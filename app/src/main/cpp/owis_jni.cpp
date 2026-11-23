// JNI bridge: the portable OWIS engine (engine/*.h) + the OpenPort 2.0 transport (op2_bus.h) exposed to
// Kotlin. The engine runs in native code; USB I/O and UI callbacks cross back to Kotlin. All native
// methods are called from a single background thread (Kotlin coroutine), which is JVM-attached, so the
// upcalls (USB read/write, log/live) use that thread's JNIEnv.
#include <jni.h>
#include <memory>
#include <string>
#include <vector>
#include "engine/engine.h"
#include "engine/sim.h"
#include "op2_bus.h"

using namespace pt;

static JavaVM *g_vm = nullptr;
static jobject g_usb = nullptr;          // Kotlin UsbLink: int write(byte[]), byte[] read(int,int)
static jobject g_cb = nullptr;           // Kotlin NativeCallbacks: onLog/onOut/onLive
static jmethodID m_write = nullptr, m_read = nullptr, m_onLog = nullptr, m_onOut = nullptr, m_onLive = nullptr;

static Engine g_en;
static std::unique_ptr<Op2Bus> g_bus;
static std::vector<PackInfo> g_packs;    // pack heads for VIN detection

static JNIEnv *env() {
    void *p = nullptr;
    if (g_vm->GetEnv(&p, JNI_VERSION_1_6) == JNI_OK) return (JNIEnv *)p;
    JNIEnv *e = nullptr;
#ifdef __ANDROID__
    g_vm->AttachCurrentThread(&e, nullptr);
#else
    g_vm->AttachCurrentThread((void **)&e, nullptr);
#endif
    return e;
}

// ---- helpers ----
static std::string jToStr(JNIEnv *e, jstring s) {
    if (!s) return "";
    const char *c = e->GetStringUTFChars(s, nullptr); std::string r = c ? c : ""; e->ReleaseStringUTFChars(s, c); return r;
}
static jstring toJ(JNIEnv *e, const std::string &s) { return e->NewStringUTF(s.c_str()); }
static std::string esc(const std::string &s) {
    std::string o; for (char c : s) {
        switch (c) { case '"': o += "\\\""; break; case '\\': o += "\\\\"; break; case '\n': o += "\\n"; break;
            case '\r': break; case '\t': o += "\\t"; break;
            default: if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += c; }
    }
    return o;
}
static std::string q(const std::string &s) { return "\"" + esc(s) + "\""; }

// ---- USB pipe backed by the Kotlin UsbLink ----
static int usbWrite(const uint8_t *d, int n) {
    JNIEnv *e = env();
    jbyteArray a = e->NewByteArray(n); e->SetByteArrayRegion(a, 0, n, (const jbyte *)d);
    int r = e->CallIntMethod(g_usb, m_write, a); e->DeleteLocalRef(a);
    return r;
}
static int usbRead(uint8_t *buf, int max, int timeoutMs) {
    JNIEnv *e = env();
    jbyteArray a = (jbyteArray)e->CallObjectMethod(g_usb, m_read, (jint)max, (jint)timeoutMs);
    if (!a) return 0;
    int n = e->GetArrayLength(a); if (n > max) n = max;
    if (n > 0) e->GetByteArrayRegion(a, 0, n, (jbyte *)buf);
    e->DeleteLocalRef(a);
    return n;
}
static void cbLog(const std::string &s) { JNIEnv *e = env(); jstring j = toJ(e, s); e->CallVoidMethod(g_cb, m_onLog, j); e->DeleteLocalRef(j); }
static void cbOut(const std::string &s) { JNIEnv *e = env(); jstring j = toJ(e, s); e->CallVoidMethod(g_cb, m_onOut, j); e->DeleteLocalRef(j); }

static void wireCallbacks() {
    g_en.onLog = [](const std::string &s) { cbLog(s); };
    g_en.onOut = [](const std::string &s) { cbOut(s); };
    g_en.onLive = [](const std::string &fn, const std::vector<pk::Value> &vs) {
        JNIEnv *e = env();
        std::vector<float> nums; std::vector<std::string> names;
        for (auto &v : vs) if (!std::isnan(v.num)) { nums.push_back((float)v.num); names.push_back(v.name + (v.unit.empty() ? "" : " " + v.unit)); }
        jfloatArray fa = e->NewFloatArray((jsize)nums.size()); if (!nums.empty()) e->SetFloatArrayRegion(fa, 0, (jsize)nums.size(), nums.data());
        jclass sc = e->FindClass("java/lang/String");
        jobjectArray na = e->NewObjectArray((jsize)names.size(), sc, nullptr);
        for (size_t i = 0; i < names.size(); i++) { jstring s = toJ(e, names[i]); e->SetObjectArrayElement(na, (jsize)i, s); e->DeleteLocalRef(s); }
        jstring jf = toJ(e, fn);
        e->CallVoidMethod(g_cb, m_onLive, jf, fa, na);
        e->DeleteLocalRef(jf); e->DeleteLocalRef(fa); e->DeleteLocalRef(na); e->DeleteLocalRef(sc);
    };
}

static std::string scanJson() {
    std::string o = "[";
    bool first = true;
    for (auto &r : g_en.scan) {
        if (!r.present) continue;
        if (!first) o += ","; first = false;
        o += "{" + std::string("\"name\":") + q(r.name) + ",\"id\":" + std::to_string(r.req) +
             ",\"proto\":" + q(r.proto == PROTO_KWP ? "KWP" : r.proto == PROTO_UDS ? "UDS" : "") +
             ",\"ident\":" + q(r.ident) + ",\"faults\":[";
        bool f1 = true;
        for (auto &f : r.faults) {
            if (!f1) o += ","; f1 = false;
            std::string status = f.d.bits == 24 ? udsStatusDetail(f.d.status) : statusText(f.d);
            o += "{\"code\":" + q(dtcName(f.d)) + ",\"text\":" + q(f.text) + ",\"status\":" + q(status) + ",\"env\":" + q(f.env) + "}";
        }
        o += "]}";
    }
    o += "]";
    return o;
}

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) { g_vm = vm; return JNI_VERSION_1_6; }

// Register the Kotlin UsbLink and NativeCallbacks objects (global refs) and cache their method ids.
JNIEXPORT void JNICALL Java_com_owis_diag_Native_nativeRegister(JNIEnv *e, jclass, jobject usb, jobject cb) {
    if (g_usb) e->DeleteGlobalRef(g_usb);
    if (g_cb) e->DeleteGlobalRef(g_cb);
    g_usb = e->NewGlobalRef(usb); g_cb = e->NewGlobalRef(cb);
    jclass uc = e->GetObjectClass(usb);
    m_write = e->GetMethodID(uc, "write", "([B)I");
    m_read = e->GetMethodID(uc, "read", "(II)[B");
    jclass cc = e->GetObjectClass(cb);
    m_onLog = e->GetMethodID(cc, "onLog", "(Ljava/lang/String;)V");
    m_onOut = e->GetMethodID(cc, "onOut", "(Ljava/lang/String;)V");
    m_onLive = e->GetMethodID(cc, "onLive", "(Ljava/lang/String;[F[Ljava/lang/String;)V");
    wireCallbacks();
}

// Load the key file (coding unlock) and the pack heads from a data directory. Returns a JSON array of packs.
JNIEXPORT jstring JNICALL Java_com_owis_diag_Native_nativeSetup(JNIEnv *e, jclass, jstring dataDir, jstring keyFile, jstring reportDir) {
    std::string dir = jToStr(e, dataDir), key = jToStr(e, keyFile), rep = jToStr(e, reportDir);
    g_en.reportDir = rep;
    if (!key.empty()) sk::load(key);
    g_packs.clear();
    // the Kotlin side passes the list of pack file paths, newline-separated, in dataDir
    std::string list = dir; size_t p = 0;
    while (p < list.size()) {
        size_t nl = list.find('\n', p); std::string path = list.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
        p = nl == std::string::npos ? list.size() : nl + 1;
        if (path.empty()) continue;
        PackInfo pi; if (readPackHead(path, pi)) g_packs.push_back(pi);
    }
    std::string o = "["; for (size_t i = 0; i < g_packs.size(); i++) { if (i) o += ",";
        o += "{\"id\":" + q(g_packs[i].id) + ",\"title\":" + q(g_packs[i].title) + ",\"path\":" + q(g_packs[i].path) + "}"; }
    o += "]";
    return toJ(e, o);
}

JNIEXPORT jint JNICALL Java_com_owis_diag_Native_nativeKeyCount(JNIEnv *, jclass) { return (jint)sk::keys().size(); }

JNIEXPORT jboolean JNICALL Java_com_owis_diag_Native_nativeConnect(JNIEnv *, jclass) {
    UsbPipe pipe; pipe.write = usbWrite; pipe.read = usbRead;
    g_bus.reset(new Op2Bus(pipe));
    g_bus->onError = [](const std::string &s) { cbLog(s); };
    g_bus->tap = nullptr;
    return g_bus->open() ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_com_owis_diag_Native_nativeDisconnect(JNIEnv *, jclass) { if (g_bus) { g_bus->close(); g_bus.reset(); } }

JNIEXPORT jboolean JNICALL Java_com_owis_diag_Native_nativeLoadPack(JNIEnv *e, jclass, jstring path) {
    std::string err; bool ok = g_en.loadPack(jToStr(e, path), err); if (!err.empty()) cbLog(err); return ok ? JNI_TRUE : JNI_FALSE;
}

// Detect the car from the VIN, load the matching pack. Returns JSON {vin,packId,title} or {} on failure.
JNIEXPORT jstring JNICALL Java_com_owis_diag_Native_nativeDetect(JNIEnv *e, jclass) {
    if (!g_bus) return toJ(e, "{}");
    std::string how; int k = g_en.detectVehicle(*g_bus, g_packs, how);
    cbLog(how);
    if (k < 0 || k >= (int)g_packs.size()) return toJ(e, "{}");
    std::string err; g_en.loadPack(g_packs[k].path, err);
    std::string o = "{\"vin\":" + q(g_en.vin) + ",\"packId\":" + q(g_packs[k].id) + ",\"title\":" + q(g_packs[k].title) + "}";
    return toJ(e, o);
}

JNIEXPORT jstring JNICALL Java_com_owis_diag_Native_nativeScan(JNIEnv *e, jclass) {
    if (!g_bus || !g_en.packOk) return toJ(e, "[]");
    g_en.stop = false; g_en.actScanAll(*g_bus);
    return toJ(e, scanJson());
}
JNIEXPORT void JNICALL Java_com_owis_diag_Native_nativeClearAll(JNIEnv *, jclass) { if (g_bus && g_en.packOk) g_en.actClearAll(*g_bus); }

JNIEXPORT jstring JNICALL Java_com_owis_diag_Native_nativeModules(JNIEnv *e, jclass) {
    std::string o = "["; for (size_t i = 0; i < g_en.pack.mods.size(); i++) { if (i) o += ",";
        o += "{\"i\":" + std::to_string(i) + ",\"key\":" + q(g_en.pack.mods[i].key) + ",\"name\":" + q(g_en.modName(g_en.pack.mods[i])) + "}"; }
    o += "]"; return toJ(e, o);
}

JNIEXPORT jstring JNICALL Java_com_owis_diag_Native_nativeFunctions(JNIEnv *e, jclass, jint mi, jstring filter) {
    g_en.selectModule(mi); std::string f = jToStr(e, filter);
    std::string o = "["; bool first = true;
    for (size_t i = 0; i < g_en.entries.size(); i++) { const Entry &en = g_en.entries[i];
        if (!f.empty() && en.name.find(f) == std::string::npos) continue;
        if (!first) o += ","; first = false;
        o += "{\"i\":" + std::to_string(i) + ",\"cat\":\"" + std::string(1, en.cat) + "\",\"name\":" + q(en.name) + "}"; }
    o += "]"; return toJ(e, o);
}

JNIEXPORT jboolean JNICALL Java_com_owis_diag_Native_nativeRun(JNIEnv *e, jclass, jint mi, jint entry, jdoubleArray inputs) {
    if (!g_bus) return JNI_FALSE;
    pk::Inputs in; if (inputs) { jsize n = e->GetArrayLength(inputs); std::vector<jdouble> v(n); e->GetDoubleArrayRegion(inputs, 0, n, v.data()); for (auto x : v) in.push_back((double)x); }
    g_en.stop = false; return g_en.runFunction(*g_bus, mi, entry, in) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_com_owis_diag_Native_nativeLive(JNIEnv *e, jclass, jint mi, jintArray sel, jint rounds) {
    if (!g_bus) return;
    std::vector<int> s; if (sel) { jsize n = e->GetArrayLength(sel); std::vector<jint> v(n); e->GetIntArrayRegion(sel, 0, n, v.data()); for (auto x : v) s.push_back((int)x); }
    g_en.stop = false; g_en.runLive(*g_bus, mi, s, {}, rounds);
}
JNIEXPORT void JNICALL Java_com_owis_diag_Native_nativeStop(JNIEnv *, jclass) { g_en.stop = true; }

JNIEXPORT jstring JNICALL Java_com_owis_diag_Native_nativeJobs(JNIEnv *e, jclass) {
    std::string o = "["; bool first = true;
    for (auto &j : g_en.pack.jobs) { if (!first) o += ","; first = false;
        o += "{\"id\":" + q(j.id) + ",\"title\":" + q(j.title) + ",\"note\":" + q(g_en.jobNote(j)) + "}"; }
    o += "]"; return toJ(e, o);
}
JNIEXPORT jboolean JNICALL Java_com_owis_diag_Native_nativeRunJob(JNIEnv *e, jclass, jstring id, jdoubleArray inputs) {
    if (!g_bus) return JNI_FALSE;
    const pk::Job *j = g_en.findJob(jToStr(e, id)); if (!j) return JNI_FALSE;
    pk::Inputs in; if (inputs) { jsize n = e->GetArrayLength(inputs); std::vector<jdouble> v(n); e->GetDoubleArrayRegion(inputs, 0, n, v.data()); for (auto x : v) in.push_back((double)x); }
    g_en.stop = false; return g_en.runJob(*g_bus, *j, {in}) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL Java_com_owis_diag_Native_nativeVin(JNIEnv *e, jclass) { return toJ(e, g_en.vin); }
JNIEXPORT jstring JNICALL Java_com_owis_diag_Native_nativeReportHtml(JNIEnv *e, jclass) { return toJ(e, g_en.lastHtml); }

} // extern "C"
