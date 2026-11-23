// OWIS engine: everything the tool does with a car, independent of the user interface and of
// the adapter. Fault memory of all control units (with environment data), module functions driven by the
// data pack, coding unlock, guided jobs, vehicle detection and reports.
// Portable C++17 - the Windows app (owis.cpp) and the command-line tool (cli.cpp) both sit on top,
// and tests/test_engine.cpp runs all of it against the simulated car.
#pragma once
#include "diag_core.h"
#include "bus.h"
#include "seedkey.h"
#include "pack.h"
#include "safety.h"
#include <algorithm>
#include <atomic>
#include <ctime>
#include <map>

#define PT_VERSION "0.5"

namespace pt {

inline std::string stampNow() {
    time_t t = time(nullptr); struct tm tmv;
#ifdef _WIN32
    tmv = *localtime(&t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[32]; strftime(b, sizeof b, "%Y%m%d_%H%M%S", &tmv); return b;
}
inline std::string htmlEsc(const std::string &s) {
    std::string o;
    for (char c : s) { if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;"; else if (c == '"') o += "&quot;"; else o += c; }
    return o;
}

// ---------------------------------------------------------------- data packs on disk
struct PackInfo { std::string path, id, title, lang; std::vector<std::string> vin, pn; };
// Reads the head of a pack file (platform, title, VIN types) without loading the whole pack.
inline bool readPackHead(const std::string &path, PackInfo &pi) {
    FILE *f = fopen(path.c_str(), "rb"); if (!f) return false;
    char head[2048] = {0}; size_t n = fread(head, 1, sizeof head - 1, f); fclose(f); head[n] = 0;
    if (!strstr(head, "\"v\":1")) return false;
    auto str = [&](const char *key) -> std::string {
        std::string k = std::string("\"") + key + "\":\""; const char *t = strstr(head, k.c_str()); if (!t) return "";
        t += k.size(); const char *e = strchr(t, '"'); return e ? std::string(t, e - t) : "";
    };
    auto list = [&](const char *key, std::vector<std::string> &out) {
        std::string k = std::string("\"") + key + "\":["; const char *t = strstr(head, k.c_str()); if (!t) return;
        t += k.size(); const char *e = strchr(t, ']'); if (!e) return;
        while (t < e) { const char *a = strchr(t, '"'); if (!a || a >= e) break; const char *b = strchr(a + 1, '"'); if (!b || b > e) break;
            out.push_back(std::string(a + 1, b - a - 1)); t = b + 1; }
    };
    pi.path = path; pi.id = str("platform"); pi.title = str("title"); pi.lang = str("lang");
    list("vin", pi.vin); list("pn", pi.pn);
    return !pi.title.empty();
}
// Pack for a VIN model type ("970"): exact match first, then a unique two-character match ("9Y" -> Cayenne 9Y0).
inline int matchPack(const std::vector<PackInfo> &packs, const std::string &type, bool partNumber = false) {
    if (type.size() < 3) return -1;
    for (int pass = 0; pass < 2; pass++) {
        int hit = -1, hits = 0; std::string lastId;
        for (int i = 0; i < (int)packs.size(); i++)
            for (auto &v : partNumber ? packs[i].pn : packs[i].vin)
                if (pass == 0 ? v == type : (v.size() >= 2 && v.compare(0, 2, type, 0, 2) == 0)) {
                    if (packs[i].id != lastId) hits++;
                    if (hit < 0 || (packs[hit].lang == "de" && packs[i].lang != "de" && packs[hit].id == packs[i].id)) hit = i;
                    lastId = packs[i].id; break;
                }
        if (hit >= 0 && (pass == 0 || hits == 1)) return hit;
    }
    return -1;
}

// ---------------------------------------------------------------- results
struct ModResult { bool present = false; int proto = 0; std::vector<Dtc> dtcs; std::string ident; };
struct FaultRow { Dtc d; std::string text, env; };
struct ScanRow { int mod = -1; uint32_t req = 0; bool present = false; int proto = 0; std::string name, ident; std::vector<FaultRow> faults; };

// One line in a module's function list: a routine groups start / stop / result, an actuator test groups
// the adjustment and "return control".
struct Entry { char cat = 'O'; std::string name; int main = -1, stop = -1, result = -1, ret = -1; };

class Engine {
public:
    pk::Pack pack; bool packOk = false;
    std::atomic<bool> stop{false};
    bool autoMode = false;                 // scripted run: bounded waits, no endless holds
    bool quick = false;                    // tests: no real-time pauses
    bool envOnScan = true;                 // read environment data of every fault during a scan
    bool redactVin = false;                // hide the VIN serial in saved reports
    std::string reportDir = ".";
    std::string vin;                       // VIN of the connected car, once read
    std::vector<ScanRow> scan; std::vector<std::string> scanSkipped;
    std::string lastTxt, lastHtml;         // files written by the last scan
    std::vector<Entry> entries;            // functions of the current module (see buildEntries)
    int curMod = -1;

    std::function<void(const std::string &)> onLog, onOut, onSetOut;
    std::function<void(const std::string &fn, const std::vector<pk::Value> &)> onLive;

    void log(const std::string &s) { if (onLog) onLog(s); }
    void out(const std::string &s) { if (onOut) onOut(s); else log(s); }
    void setOut(const std::string &s) { if (onSetOut) onSetOut(s); }
    void nap(unsigned ms) { sleepMs(quick ? 1 : ms); }

    bool loadPack(const std::string &path, std::string &err) {
        packOk = false; pack = pk::Pack(); curMod = -1; entries.clear();
        FILE *f = fopen(path.c_str(), "rb"); if (!f) { err = "cannot open " + path; return false; }
        std::string text; char buf[65536]; size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
        fclose(f);
        if (!pack.load(text, err)) return false;
        packOk = true; return true;
    }
    size_t functionCount() const { size_t n = 0; for (auto &m : pack.mods) n += m.svc.size(); return n; }
    std::string modName(const pk::Module &m) const { std::string n = pack.S(m.name); return n.empty() ? m.key : n; }
    int findModule(const std::string &key) const { for (int i = 0; i < (int)pack.mods.size(); i++) if (pack.mods[i].key == key) return i; return -1; }
    static int linkOf(const pk::Module &m) { return m.link == 1 ? LINK_KLINE : m.link == 2 ? LINK_TP20 : LINK_CAN; }
    static bool kwpOnly(const Bus &b) { return b.kind == LINK_KLINE || b.kind == LINK_TP20; }
    static bool link(Bus &b, const pk::Module &m, const std::pair<int, int> &id) {
        b.klInit = m.klInit; b.kl5Baud = m.kl5Baud; b.klBaud = m.klBaud;
        return b.target((uint32_t)id.first, (uint32_t)id.second, linkOf(m));
    }

    // ------------------------------------------------------------ fault memory
    void identUds(Bus &b, std::string &id) {
        static const struct { uint16_t did; const char *lbl; } D[] = {{0xF187, "part no."}, {0xF189, "SW"}, {0xF191, "HW"}, {0xF197, "system"}};
        std::vector<uint8_t> r;
        for (auto &d : D)
            if (b.xfer({0x22, (uint8_t)(d.did >> 8), (uint8_t)d.did}, r, 800) && r.size() > 3) {
                std::string v = printable(r.data() + 3, r.size() - 3);
                if (!v.empty()) { if (!id.empty()) id += " | "; id += std::string(d.lbl) + " " + v; }
            }
    }
    void identKwp(Bus &b, std::string &id) {
        std::vector<uint8_t> r;
        if (b.xfer({0x1A, 0x9B}, r, 800) && r.size() > 2) id = printable(r.data() + 2, r.size() - 2);
        else if (b.xfer({0x1A, 0x87}, r, 800) && r.size() > 2) id = printable(r.data() + 2, r.size() - 2);
    }
    bool readFaults(Bus &b, int proto, ModResult &mr) {
        std::vector<uint8_t> r;
        auto tryUds = [&]() -> bool {
            if (b.xfer({0x19, 0x02, 0xFF}, r, 800) && parseUds1902(r, mr.dtcs)) { mr.present = true; mr.proto = PROTO_UDS; return true; }
            if (b.gotAnswer) mr.present = true;
            return false;
        };
        auto tryKwp = [&]() -> bool {
            std::vector<uint8_t> s; b.xfer({0x10, 0x81}, s, 600);
            if (b.gotAnswer) mr.present = true;
            if (b.xfer({0x18, 0x00, 0xFF, 0x00}, r, 1500) && parseKwp18(r, mr.dtcs)) { mr.present = true; mr.proto = PROTO_KWP; return true; }
            if (b.gotAnswer) mr.present = true;
            if (b.xfer({0x18, 0x02, 0xFF, 0x00}, r, 1500) && parseKwp18(r, mr.dtcs)) { mr.present = true; mr.proto = PROTO_KWP; return true; }
            if (b.gotAnswer) mr.present = true;
            return false;
        };
        if (kwpOnly(b)) return tryKwp();
        if (proto == PROTO_KWP) { if (tryKwp()) return true; return mr.present && tryUds(); }
        if (tryUds()) return true;
        if (!mr.present && proto == PROTO_UDS) return false;
        return tryKwp();
    }
    bool clearFaults(Bus &b, int proto) {
        std::vector<uint8_t> r, s;
        if (proto == PROTO_UDS) {
            if (b.xfer({0x14, 0xFF, 0xFF, 0xFF}, r, 5000)) return true;
            b.xfer({0x10, 0x03}, s, 1000);
            bool ok = b.xfer({0x14, 0xFF, 0xFF, 0xFF}, r, 5000);
            uint8_t nrc = b.lastNrc; b.xfer({0x10, 0x01}, s, 1000); b.lastNrc = nrc;
            return ok;
        }
        if (b.xfer({0x14, 0xFF, 0x00}, r, 5000)) return true;
        b.xfer({0x10, 0x89}, s, 1000);
        bool ok = b.xfer({0x14, 0xFF, 0x00}, r, 5000);
        uint8_t nrc = b.lastNrc; b.xfer({0x10, 0x81}, s, 1000); b.lastNrc = nrc;
        return ok;
    }
    std::string dtcTextOf(const pk::Module &m, const Dtc &d) const {
        const pk::Dtc *alt = nullptr;
        for (auto &x : m.dtc) {
            if (x.bits == d.bits && x.code == d.code) return pack.S(x.text);
            if (d.bits == 24 && x.bits == 16 && x.code == (d.code >> 8)) alt = &x;
        }
        return alt ? pack.S(alt->text) : "(code not in catalogue)";
    }

    // Environment data of one fault (UDS 19 06: how often, when, at what mileage and voltage it was stored).
    // Decoded with the layout from the data pack; whatever the pack does not describe is shown as hex.
    static std::string formatEnv(const std::vector<pk::Value> &vs) {
        std::string o; int Y = -1, Mo = -1, D = -1, h = -1, mi = -1, se = -1; bool dateDone = false;
        auto add = [&](const std::string &x) { if (!o.empty()) o += " | "; o += x; };
        auto num = [](const pk::Value &v) { return (int)(std::isnan(v.num) ? (std::isnan(v.raw) ? -1 : v.raw) : v.num); };
        auto flush = [&]() {
            if (dateDone || Y < 0) return;
            dateDone = true;
            if (Y < 100) Y += 2000;
            if (Mo < 1 || Mo > 12 || D < 1 || D > 31) return;            // no valid clock in this control unit
            char b[40]; snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", Y, Mo, D, std::max(0, h), std::max(0, mi), std::max(0, se)); add(b);
        };
        for (auto &v : vs) {
            if (v.name == "Year") { Y = num(v); continue; }
            if (v.name == "Month") { Mo = num(v); continue; }
            if (v.name == "Day") { D = num(v); continue; }
            if (v.name == "Hour") { h = num(v); continue; }
            if (v.name == "Minute") { mi = num(v); continue; }
            if (v.name == "Second") { se = num(v); flush(); continue; }
            if (v.text.empty()) continue;
            // A control unit that never recorded this freeze-frame field reads it back as all-ones (0xFF...).
            // Don't scale that into a nonsense value (16777215 km, 51 V); show it as not recorded. Multi-byte
            // sentinels are unambiguous; for a single byte only voltage qualifies (51 V is physically impossible),
            // so genuine 1-byte counters that legitimately reach 255 are left untouched.
            double rw = v.raw;
            bool unset = (rw == 65535.0 || rw == 16777215.0 || rw == 4294967295.0) ||
                         (rw == 255.0 && (v.unit == "V" || v.unit == "Volt"));
            if (unset) { add(v.name + " not recorded"); continue; }
            add(v.name + " " + v.text + (v.unit.empty() ? "" : " " + v.unit));
        }
        flush();
        return o;
    }
    std::string readEnv(Bus &b, const pk::Module &m, const Dtc &d) {
        if (d.bits != 24) return "";
        std::vector<uint8_t> r;
        if (!b.xfer({0x19, 0x06, (uint8_t)(d.code >> 16), (uint8_t)(d.code >> 8), (uint8_t)d.code, 0xFF}, r, 1500) || r.size() < 7) return "";
        const pk::Service *es = nullptr; for (auto &s : m.svc) if (s.cls == "E") { es = &s; break; }
        std::vector<pk::Value> vals; size_t covered = 6;
        if (es) covered = std::max<size_t>(6, pack.decodePartial(*es, r, vals));
        std::string o = formatEnv(vals);
        if (vals.empty() && r.size() > 6) o = "data " + hexStr(r.data() + 6, r.size() - 6);
        else if (covered < r.size()) o += std::string(o.empty() ? "" : " | ") + "more data " + hexStr(r.data() + covered, r.size() - covered);
        return o;
    }

    // Snapshot (freeze frame) of one fault, UDS 19 04 <DTC> FF:
    //   59 04 <DTC 3> <status> { <record no.> <count> { <DID 2> <data> } }
    // Each data identifier is decoded with the module's own read function for that identifier (22 <DID>),
    // which also gives its length. At the first identifier the pack does not describe, the rest is shown as hex.
    std::string readSnapshot(Bus &b, const pk::Module &m, const Dtc &d) {
        if (d.bits != 24) return "";
        std::vector<uint8_t> r;
        if (!b.xfer({0x19, 0x04, (uint8_t)(d.code >> 16), (uint8_t)(d.code >> 8), (uint8_t)d.code, 0xFF}, r, 1500) || r.size() < 8) return "";
        std::string o; size_t p = 6;
        auto add = [&](const std::string &x) { if (!o.empty()) o += " | "; o += x; };
        while (p + 2 <= r.size()) {
            int rec = r[p], cnt = r[p + 1]; p += 2;
            if (rec != 1) add("record " + std::to_string(rec));
            for (int k = 0; k < cnt && p + 2 <= r.size(); k++) {
                uint16_t did = (uint16_t)(r[p] << 8 | r[p + 1]);
                const pk::Service *rs = nullptr;
                for (auto &s : m.svc) { double sid = -1, key = -1; if (constAt(s, 0, sid) && sid == 0x22 && constAt(s, 1, key) && key == did) { rs = &s; break; } }
                if (!rs) { char t[16]; snprintf(t, sizeof t, "%04X", did); add(std::string("data ") + t + " " + hexStr(r.data() + p + 2, r.size() - p - 2)); return o; }
                std::vector<uint8_t> fake = {0x62, r[p], r[p + 1]}; fake.insert(fake.end(), r.begin() + p + 2, r.end());
                std::vector<pk::Value> vals; size_t used = pack.decodePartial(*rs, fake, vals);
                if (used <= 3) { add("data " + hexStr(r.data() + p, r.size() - p)); return o; }
                for (auto &v : vals) if (!v.text.empty()) add(v.name + " " + v.text + (v.unit.empty() ? "" : " " + v.unit));
                p += used - 1;                              // 2 DID bytes + data
            }
        }
        return o;
    }

    bool scanOne(Bus &b, int mi, bool withIdent, ScanRow &row, ModResult *res = nullptr) {
        const pk::Module &m = pack.mods[mi]; ModResult mr;
        row = ScanRow(); row.mod = mi; row.name = modName(m); row.req = m.ids.empty() ? 0 : (uint32_t)m.ids[0].first;
        for (auto &pp : m.ids) {
            if (stop) break;
            if (!link(b, m, pp)) return false;
            if (readFaults(b, m.proto, mr) || mr.present) {
                if (withIdent) { if (mr.proto == PROTO_KWP) identKwp(b, mr.ident); else identUds(b, mr.ident); }
                row.req = (uint32_t)pp.first; row.present = true; row.proto = mr.proto; row.ident = mr.ident;
                int n = 0;
                for (const Dtc &d : mr.dtcs) {
                    FaultRow fr; fr.d = d; fr.text = dtcTextOf(m, d);
                    if (envOnScan && mr.proto == PROTO_UDS && n++ < 25 && !stop) {
                        fr.env = readEnv(b, m, d);
                        std::string snap = readSnapshot(b, m, d);
                        if (!snap.empty()) fr.env += (fr.env.empty() ? "" : " | ") + std::string("snapshot: ") + snap;
                    }
                    row.faults.push_back(fr);
                }
                if (res) *res = mr;
                return true;
            }
        }
        if (res) *res = mr;
        return false;
    }
    std::string rowHeader(const ScanRow &r) const {
        char hdr[64]; int lk = r.mod >= 0 && r.mod < (int)pack.mods.size() ? pack.mods[r.mod].link : 0;
        snprintf(hdr, sizeof hdr, lk == 1 ? "[K-line %02X] " : lk == 2 ? "[TP2.0 %02X] " : r.req > 0x7FF ? "[%08X] " : "[%03X] ", (unsigned)r.req);
        std::string n = r.name; if (n.size() < 40) n.append(40 - n.size(), ' ');
        return hdr + n;
    }
    void printRow(const ScanRow &r, bool toReport, bool detail = false) {
        auto p = [&](const std::string &s) { if (toReport) rep(s); else out(s); };
        bool unconf = r.mod >= 0 && r.mod < (int)pack.mods.size() && !pack.mods[r.mod].addrConfirmed;
        if (!r.present) { p(rowHeader(r) + " -- no answer (not fitted or asleep)" + (unconf ? " [address not yet confirmed on a car]" : "")); return; }
        p(rowHeader(r) + (r.proto == PROTO_UDS ? " UDS  " : " KWP  ") + std::to_string(r.faults.size()) + " fault(s)");
        if (!r.ident.empty()) p("        " + r.ident);
        for (auto &f : r.faults) {
            p("    * " + dtcName(f.d) + "  " + f.text + "   [" + statusText(f.d) + "]");
            if (detail && f.d.bits == 24) p("          status: " + udsStatusDetail(f.d.status));
            if (!f.env.empty()) p("          stored: " + f.env);
        }
    }

    // ------------------------------------------------------------ reports
    FILE *report = nullptr;
    void rep(const std::string &s) { log(s); if (report) fprintf(report, "%s\r\n", s.c_str()); }
    std::string openReport(const char *what) {
        std::string path = reportDir + "/OWIS_" + what + "_" + stampNow() + ".txt";
        report = fopen(path.c_str(), "wb");
        if (report) log("Report: " + path);
        return report ? path : "";
    }
    void closeReport() { if (report) { fclose(report); report = nullptr; } }
    std::string vinShown() const { return vin.empty() ? "" : (redactVin ? vinRedact(vin) : vin); }

    bool writeHtml(const std::string &path) const {
        FILE *f = fopen(path.c_str(), "wb"); if (!f) return false;
        int found = 0, faults = 0; for (auto &r : scan) if (r.present) { found++; faults += (int)r.faults.size(); }
        std::string st = stampNow();
        std::string date = st.substr(0, 4) + "-" + st.substr(4, 2) + "-" + st.substr(6, 2) + " " + st.substr(9, 2) + ":" + st.substr(11, 2);
        fprintf(f, "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                   "<title>Vehicle scan report</title><style>"
                   ":root{--bg:#fff;--fg:#1a1a1a;--mut:#666;--line:#ddd;--ok:#1a7f37;--bad:#b42318;--card:#f6f6f6}"
                   "@media(prefers-color-scheme:dark){:root{--bg:#161616;--fg:#eee;--mut:#aaa;--line:#333;--ok:#4ac26b;--bad:#ff7b72;--card:#202020}}"
                   "body{font:15px/1.45 -apple-system,Segoe UI,Roboto,Arial,sans-serif;background:var(--bg);color:var(--fg);max-width:980px;margin:0 auto;padding:24px 16px}"
                   "h1{font-size:22px;margin:0 0 4px}h2{font-size:16px;margin:28px 0 8px}.mut{color:var(--mut)}"
                   ".sum{display:flex;gap:12px;flex-wrap:wrap;margin:16px 0}.sum div{background:var(--card);border-radius:8px;padding:10px 16px}"
                   ".sum b{display:block;font-size:22px}table{border-collapse:collapse;width:100%%}td,th{text-align:left;padding:6px 8px;border-bottom:1px solid var(--line);vertical-align:top}"
                   "th{font-size:12px;text-transform:uppercase;letter-spacing:.04em;color:var(--mut)}.ok{color:var(--ok)}.bad{color:var(--bad);font-weight:600}"
                   ".mod{margin:14px 0;border:1px solid var(--line);border-radius:8px;padding:10px 14px;break-inside:avoid}.mod h3{margin:0;font-size:15px}"
                   "code{font:13px ui-monospace,Consolas,monospace}.env{color:var(--mut);font-size:13px}"
                   "@media print{body{max-width:none;padding:0}.mod{border-color:#bbb}}"
                   "</style></head><body>\n");
        fprintf(f, "<h1>Vehicle scan report</h1><div class=\"mut\">%s", htmlEsc(pack.title).c_str());
        if (!vin.empty()) fprintf(f, " &middot; VIN <code>%s</code>", htmlEsc(vinShown()).c_str());
        fprintf(f, " &middot; %s &middot; OWIS %s</div>\n", date.c_str(), PT_VERSION);
        fprintf(f, "<div class=\"sum\"><div><b>%d</b>control units answering</div><div><b class=\"%s\">%d</b>fault(s) stored</div><div><b>%d</b>not answering</div></div>\n",
                found, faults ? "bad" : "ok", faults, (int)scan.size() - found);
        fprintf(f, "<h2>Control units with faults</h2>\n");
        bool any = false;
        for (auto &r : scan) if (r.present && !r.faults.empty()) {
            any = true;
            fprintf(f, "<div class=\"mod\"><h3>%s <span class=\"mut\">&middot; %X &middot; %s</span></h3>", htmlEsc(r.name).c_str(), (unsigned)r.req, r.proto == PROTO_UDS ? "UDS" : "KWP2000");
            if (!r.ident.empty()) fprintf(f, "<div class=\"env\">%s</div>", htmlEsc(r.ident).c_str());
            fprintf(f, "<table><tr><th>Code</th><th>Fault</th><th>Status</th></tr>\n");
            for (auto &x : r.faults) {
                fprintf(f, "<tr><td><code>%s</code></td><td>%s", dtcName(x.d).c_str(), htmlEsc(x.text).c_str());
                if (!x.env.empty()) fprintf(f, "<div class=\"env\">Stored: %s</div>", htmlEsc(x.env).c_str());
                fprintf(f, "</td><td>%s</td></tr>\n", htmlEsc(x.d.bits == 24 ? udsStatusDetail(x.d.status) : statusText(x.d)).c_str());
            }
            fprintf(f, "</table></div>\n");
        }
        if (!any) fprintf(f, "<p class=\"ok\">No faults stored in any control unit.</p>\n");
        fprintf(f, "<h2>All control units</h2><table><tr><th>Control unit</th><th>Address</th><th>Protocol</th><th>Faults</th><th>Identification</th></tr>\n");
        for (auto &r : scan) {
            if (r.present) fprintf(f, "<tr><td>%s</td><td><code>%X</code></td><td>%s</td><td class=\"%s\">%d</td><td class=\"env\">%s</td></tr>\n", htmlEsc(r.name).c_str(),
                                   (unsigned)r.req, r.proto == PROTO_UDS ? "UDS" : "KWP2000", r.faults.empty() ? "ok" : "bad", (int)r.faults.size(), htmlEsc(r.ident).c_str());
            else fprintf(f, "<tr class=\"mut\"><td>%s</td><td><code>%X</code></td><td colspan=\"3\">no answer (not fitted or asleep)</td></tr>\n", htmlEsc(r.name).c_str(), (unsigned)r.req);
        }
        fprintf(f, "</table>\n");
        if (!scanSkipped.empty()) {
            fprintf(f, "<p class=\"mut\">Not reachable directly on the diagnostic connector (sub-bus units): ");
            for (size_t i = 0; i < scanSkipped.size(); i++) fprintf(f, "%s%s", i ? ", " : "", htmlEsc(scanSkipped[i]).c_str());
            fprintf(f, "</p>\n");
        }
        fprintf(f, "<p class=\"mut\">Faults that come straight back after clearing are still present. To keep this report as a PDF, print it from the browser.</p></body></html>\n");
        fclose(f); return true;
    }

    void actScanAll(Bus &b) {
        lastTxt = openReport("scan"); lastHtml.clear(); scan.clear(); scanSkipped.clear();
        if (vin.empty()) vin = readVin(b);
        rep("=== SCAN ALL MODULES - " + pack.title + " - " + stampNow() + " ===");
        if (!vin.empty()) rep("VIN " + vinShown());
        int found = 0, faults = 0;
        for (int i = 0; i < (int)pack.mods.size(); i++) {
            if (stop) break;
            const pk::Module &m = pack.mods[i];
            if (m.ids.empty()) { scanSkipped.push_back(modName(m)); continue; }
            ScanRow row; scanOne(b, i, true, row);
            printRow(row, true);
            if (row.present) { found++; faults += (int)row.faults.size(); }
            scan.push_back(row);
        }
        rep("");
        rep("Modules answering: " + std::to_string(found) + "   Faults: " + std::to_string(faults));
        if (!scanSkipped.empty()) {
            std::string t = "Not reachable directly on the diagnostic CAN (sub-bus): ";
            for (size_t i = 0; i < scanSkipped.size(); i++) t += (i ? ", " : "") + scanSkipped[i];
            rep(t);
        }
        closeReport();
        if (!lastTxt.empty() && !stop) {
            std::string h = lastTxt.substr(0, lastTxt.size() - 4) + ".html";
            if (writeHtml(h)) { lastHtml = h; log("Report (HTML, printable): " + h); }
        }
    }
    void actClearAll(Bus &b) {
        openReport("clear");
        rep("=== CLEAR ALL FAULTS - " + pack.title + " ===");
        for (auto &m : pack.mods) {
            if (stop) break;
            for (auto &pp : m.ids) {
                ModResult mr;
                if (!link(b, m, pp)) break;
                readFaults(b, m.proto, mr);
                if (!mr.present) continue;
                bool ok = clearFaults(b, mr.proto ? mr.proto : m.proto);
                rep(std::string(ok ? "[OK]    " : "[FAIL]  ") + modName(m) + (ok ? "" : "  (" + nrcText(b.lastNrc) + ")"));
                break;
            }
        }
        rep("Done. Run SCAN again: faults that come straight back are real and still present.");
        closeReport();
    }
    // Read (and optionally clear) the fault memory of one module, with the full status and environment detail.
    bool moduleFaults(Bus &b, int mi, bool clear) {
        if (mi < 0 || mi >= (int)pack.mods.size()) return false;
        const pk::Module &m = pack.mods[mi]; ScanRow row; ModResult mr;
        if (!scanOne(b, mi, true, row, &mr)) { out("Module does not answer."); return false; }
        printRow(row, false, true);
        if (clear) {
            bool ok = clearFaults(b, mr.proto ? mr.proto : m.proto);
            out(ok ? "[OK] fault memory cleared" : "[REFUSED] " + nrcText(b.lastNrc));
            return ok;
        }
        return true;
    }

    // ------------------------------------------------------------ vehicle detection
    // VIN from the first control unit that gives one. Tried on the addresses every supported line shares.
    std::string readVin(Bus &b) {
        static const struct { uint32_t tx, rx; } P[] = {{0x7E0, 0x7E8}, {0x710, 0x77A}, {0x714, 0x77E}, {0x713, 0x77D}, {0x715, 0x77F},
                                                         {0x17FC0076, 0x17FE0076}, {0x17FC0010, 0x17FE0010}};
        std::vector<uint8_t> r;
        for (auto &p : P) {
            if (stop) break;
            if (!b.target(p.tx, p.rx, LINK_CAN)) continue;
            if (b.xfer({0x22, 0xF1, 0x90}, r, 500) && r.size() >= 20) { std::string v = vinFromBytes(r.data() + 3, r.size() - 3); if (!v.empty()) return v; }
            if (!b.gotAnswer) continue;                                              // nobody at this address
            if (b.xfer({0x1A, 0x90}, r, 500) && r.size() >= 19) { std::string v = vinFromBytes(r.data() + 2, r.size() - 2); if (!v.empty()) return v; }
            if (p.tx == 0x7E0 && b.xfer({0x09, 0x02}, r, 500) && r.size() >= 19) { std::string v = vinFromBytes(r.data() + 2, r.size() - 2); if (!v.empty()) return v; }
        }
        return "";
    }
    // Without a VIN: read the part numbers of whatever answers and take the model type most of them start
    // with (Porsche part numbers begin with the model type, e.g. 970.618...). Returns "" when undecided.
    std::string identifyByPartNumbers(Bus &b, const std::vector<PackInfo> &packs) {
        static const struct { uint32_t tx, rx; } P[] = {{0x710, 0x77A}, {0x714, 0x77E}, {0x713, 0x77D}, {0x715, 0x77F}, {0x70E, 0x778}, {0x70C, 0x776},
                                                         {0x712, 0x77C}, {0x70B, 0x775}, {0x746, 0x7B0}, {0x70F, 0x779}, {0x755, 0x7BF}, {0x7E0, 0x7E8}};
        std::map<std::string, int> votes; std::vector<uint8_t> r;
        for (auto &p : P) {
            if (stop) break;
            if (!b.target(p.tx, p.rx, LINK_CAN)) continue;
            std::string pn;
            if (b.xfer({0x22, 0xF1, 0x87}, r, 500) && r.size() > 3) pn = printable(r.data() + 3, r.size() - 3);
            else if (b.gotAnswer && b.xfer({0x1A, 0x9B}, r, 500) && r.size() > 2) pn = printable(r.data() + 2, r.size() - 2);
            std::string c; for (char ch : pn) if (ch != ' ' && ch != '.' && ch != '-') c += ch;
            if (c.size() < 3) continue;
            int k = matchPack(packs, c.substr(0, 3), true);
            if (k >= 0) votes[packs[k].id]++;
        }
        std::string best; int bn = 0, second = 0;
        for (auto &v : votes) { if (v.second > bn) { second = bn; bn = v.second; best = v.first; } else if (v.second > second) second = v.second; }
        return bn > 0 && bn > second ? best : "";
    }
    // Which pack fits the connected car? Returns an index into 'packs' or -1; 'how' says what decided it.
    int detectVehicle(Bus &b, const std::vector<PackInfo> &packs, std::string &how) {
        vin = readVin(b);
        if (!vin.empty()) {
            std::string type = vinModelType(vin);
            int k = matchPack(packs, type);
            how = "VIN " + vinShown() + (type.empty() ? "" : "  (model type " + type + (vinModelYear(vin) ? ", model year " + std::to_string(vinModelYear(vin)) : "") + ")");
            if (k >= 0) return k;
            how += " - no data pack for this model type";
        } else how = "no VIN answer";
        std::string id = identifyByPartNumbers(b, packs);
        if (!id.empty())
            for (int i = 0; i < (int)packs.size(); i++) if (packs[i].id == id && packs[i].lang != "de") { how += "; identified by control-unit part numbers"; return i; }
        if (!id.empty())
            for (int i = 0; i < (int)packs.size(); i++) if (packs[i].id == id) { how += "; identified by control-unit part numbers"; return i; }
        return -1;
    }

    // ------------------------------------------------------------ module functions (data-pack driven)
    static int sidOf(const pk::Service &s) { for (auto &p : s.rq) if (p.hasConst && p.byte == 0) return (int)p.cval; return -1; }
    static bool constAt(const pk::Service &s, int byte, double &v) { for (auto &p : s.rq) if (p.hasConst && p.byte == byte) { v = p.cval; return true; } return false; }
    static std::string stripSuffix(std::string n) {
        static const char *suf[] = {" Request Routine Results", " RequestRoutineResults", " RequestRoutineResult", " Start Routine", " StartRoutine",
            " Stop Routine", " StopRoutine", " Short Term Adjustment", " Return Control To Ecu", " Return Control To ECU",
            " Freeze Current State", " Read Data By Identifier", " ReadDataByIdentifier", " Read Read", " Read", " Write"};
        for (const char *s : suf) { size_t l = strlen(s); if (n.size() > l && n.compare(n.size() - l, l, s) == 0) return n.substr(0, n.size() - l); }
        return n;
    }
    std::vector<Entry> buildEntries(const pk::Module &m) const {
        std::vector<Entry> es; std::map<std::string, int> grp;
        for (int i = 0; i < (int)m.svc.size(); i++) {
            const pk::Service &s = m.svc[i];
            if (s.cls == "E") continue;                // environment-data layout, used by the fault reader
            int sid = sidOf(s); double sub = -1, key = -1;
            std::string gk; int role = 0;          // 0 main, 1 stop, 2 result, 3 return-control
            double vw = -1;
            if ((sid == 0x31 || sid == 0x32) && constAt(s, 1, vw) && (vw == 0xB8 || vw == 0xB9 || vw == 0xBA) && constAt(s, 2, key)) {
                // VW2000 routines by local identifier: 31 B8 <id> start, 31 BA <id> result, 32 B8 <id> stop;
                // 31 B9 <id> "continue" is a manual step of the procedure and stays a separate function
                gk = "V" + std::to_string((long long)key);
                if (vw == 0xB9) gk += "c";
                role = sid == 0x32 ? 1 : vw == 0xBA ? 2 : 0;
            } else if (sid == 0x31 && m.proto != PROTO_KWP && constAt(s, 1, sub) && sub >= 1 && sub <= 3 && constAt(s, 2, key)) {
                constAt(s, 2, key); gk = "R" + std::to_string((long long)key);
                role = sub == 2 ? 1 : sub == 3 ? 2 : 0;
            } else if (sid == 0x31 || sid == 0x32 || sid == 0x33) {
                constAt(s, 1, key); gk = "R" + std::to_string((long long)key); role = sid == 0x32 ? 1 : sid == 0x33 ? 2 : 0;
            } else if (sid == 0x2F && constAt(s, 1, key)) {
                double ctl = -1; constAt(s, 3, ctl); gk = "A" + std::to_string((long long)key);
                role = ctl == 0 ? 3 : ctl == 2 ? -1 : 0;
            } else if (sid == 0x30 && constAt(s, 1, key)) {
                double ctl = -1; constAt(s, 2, ctl); gk = "A" + std::to_string((long long)key); role = ctl == 0 ? 3 : 0;
            }
            if (role < 0) continue;                // "freeze current state" is not offered
            int e;
            if (!gk.empty() && grp.count(gk)) e = grp[gk];
            else {
                e = (int)es.size(); es.push_back(Entry());
                es[e].cat = s.cls.empty() ? 'O' : s.cls[0];
                if (!gk.empty()) grp[gk] = e;
            }
            Entry &en = es[e];
            if (role == 0 && en.main < 0) { en.main = i; en.name = stripSuffix(pack.S(s.name)); en.cat = s.cls.empty() ? 'O' : s.cls[0]; }
            else if (role == 1) en.stop = i; else if (role == 2) en.result = i; else if (role == 3) en.ret = i;
            if (en.name.empty()) en.name = stripSuffix(pack.S(s.name));
        }
        // routines that only have a result (status read) become readable entries
        for (auto &e : es) if (e.main < 0 && e.result >= 0) e.main = e.result;
        es.erase(std::remove_if(es.begin(), es.end(), [](const Entry &e) { return e.main < 0; }), es.end());
        std::stable_sort(es.begin(), es.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
        return es;
    }
    void selectModule(int mi) { curMod = mi; entries = mi >= 0 && mi < (int)pack.mods.size() ? buildEntries(pack.mods[mi]) : std::vector<Entry>(); }

    // session handling: enter the extended session when the service does not run in the default one
    void ensureSession(Bus &b, const pk::Service &s, int proto) {
        if (s.sess & pk::S_DEFAULT) return;
        std::vector<uint8_t> r;
        if (proto == PROTO_KWP) b.xfer({0x10, 0x89}, r, 1500); else b.xfer({0x10, 0x03}, r, 1500);
    }
    void testerPresent(Bus &b, int proto) {
        std::vector<uint8_t> r;
        if (proto == PROTO_KWP) b.xfer({0x3E, 0x01}, r, 300); else b.send({0x3E, 0x80});
    }
    bool connectModule(Bus &b, const pk::Module &m, int &proto) {
        for (auto &pp : m.ids) {
            if (!link(b, m, pp)) return false;
            std::vector<uint8_t> r;
            if (m.proto != PROTO_KWP && !kwpOnly(b)) { b.xfer({0x10, 0x01}, r, 600); if (b.gotAnswer) { proto = PROTO_UDS; return true; } }
            b.xfer({0x10, 0x81}, r, 600);
            if (b.gotAnswer) { proto = (b.lastNrc == 0x12 || b.lastNrc == 0x11) && !kwpOnly(b) ? PROTO_UDS : PROTO_KWP; return true; }
        }
        return false;
    }
    void printValues(const std::vector<pk::Value> &vs) {
        for (auto &v : vs) {
            std::string ind((size_t)std::max(0, v.depth - 1) * 2, ' ');
            if (v.text.empty() && v.unit.empty()) out(ind + v.name + ":");
            else out(ind + v.name + " = " + v.text + (v.unit.empty() ? "" : " " + v.unit));
        }
    }

    // Unlock coding security access for a module with the local key file. Returns true if the module is
    // unlocked (or was already open); otherwise 'why' holds a readable reason.
    // The key is computed locally. A wrong key costs one of the control unit's unlock attempts, so nothing
    // is ever sent on a guess: no mapped algorithm, a seed of unexpected length or a missing key file all
    // end here with a message and without a key on the bus.
    bool securityUnlock(Bus &b, const pk::Module &m, int proto, std::string &why) {
        if (m.sk.empty() && (m.sfd || pack.sfd)) {
            why = "this vehicle line protects coding with online authentication (SFD): the unlock comes from the "
                  "manufacturer's server and is not available offline. Fault memory, live data, tests and routines are not affected";
            return false;
        }
        if (m.sk.empty()) { why = "no coding key mapped for this control unit"; return false; }
        const sk::Key *k = sk::find(m.sk);
        if (!k) {
            why = sk::loaded() ? ("algorithm " + m.sk + " is not in the key file")
                               : "key file owis.keys not found - coding writes disabled";
            return false;
        }
        if (m.saBytes && m.saBytes != k->nb) { why = "key length of " + m.sk + " does not fit this control unit - not sending a key"; return false; }
        std::vector<uint8_t> r;
        if (proto == PROTO_KWP) b.xfer({0x10, 0x89}, r, 1500); else b.xfer({0x10, 0x03}, r, 1500);
        std::vector<uint8_t> levels;
        if (m.saLevel) levels.push_back((uint8_t)m.saLevel);            // the coding level named by the data pack
        else { levels.push_back(0x03); levels.push_back(0x01); }        // older packs: coding level, then the generic one
        for (uint8_t lvl : levels) {
            if (!b.xfer({0x27, lvl}, r, 2000)) {
                if ((b.lastNrc == 0x12 || b.lastNrc == 0x11) && levels.size() > 1) continue;      // level unknown -> try the other
                why = b.gotAnswer ? "seed request refused (" + nrcText(b.lastNrc) + ")" : "no answer to the seed request";
                return false;
            }
            std::vector<uint8_t> seed(r.begin() + (r.size() >= 2 ? 2 : r.size()), r.end());
            bool allzero = !seed.empty(); for (uint8_t x : seed) if (x) { allzero = false; break; }
            if (seed.empty() || allzero) return true;           // already unlocked
            if ((int)seed.size() < k->nb) { why = "seed is shorter than " + m.sk + " expects - not sending a key"; return false; }
            if ((int)seed.size() > k->nb) {
                if (m.saBytes == 0) { why = "seed length does not match " + m.sk + " - not sending a key"; return false; }
                seed.resize(k->nb);
            }
            std::vector<uint8_t> key;
            if (!sk::compute(*k, seed, key)) { why = "key computation failed for " + m.sk; return false; }
            std::vector<uint8_t> req = {0x27, (uint8_t)(lvl + 1)};
            req.insert(req.end(), key.begin(), key.end());
            if (b.xfer(req, r, 2000)) return true;              // {0x67, lvl+1} = unlocked
            why = b.gotAnswer ? "key rejected by control unit (" + nrcText(b.lastNrc) + ") - not retrying" : "no answer to the key";
            return false;
        }
        why = "control unit did not offer the coding security level"; return false;
    }

    // run one service; returns true on a positive answer
    bool runService(Bus &b, const pk::Module &m, int si, int proto, const pk::Inputs &in, std::vector<pk::Value> &vals, bool quiet = false) {
        const pk::Service &s = m.svc[si];
        std::vector<uint8_t> rq, r; std::string err;
        if (!pack.buildRequest(s, in, rq, err)) { out("[ERROR] " + err); return false; }
        ensureSession(b, s, proto);
        bool triedUnlock = false;
        if (s.sess & 0x100) {                                                          // service needs the coding unlock
            std::string why; triedUnlock = true;
            if (securityUnlock(b, m, proto, why)) { if (!quiet) out("[UNLOCKED] security access granted (" + m.sk + ")"); }
            else if (!quiet) out("[LOCKED] " + why);
        }
        if (!quiet) out("> " + hexStr(rq));
        bool ok = b.xfer(rq, r, 3000);
        if (!ok && (b.lastNrc == 0x7F || b.lastNrc == 0x7E || b.lastNrc == 0x80)) {   // retry in the extended session
            std::vector<uint8_t> x; if (proto == PROTO_KWP) b.xfer({0x10, 0x89}, x, 1500); else b.xfer({0x10, 0x03}, x, 1500);
            ok = b.xfer(rq, r, 3000);
        }
        if (!ok && b.lastNrc == 0x33 && !triedUnlock) {                               // security denied: unlock and retry once
            std::string why;
            if (securityUnlock(b, m, proto, why)) {
                if (!quiet) out("[UNLOCKED] security access granted (" + m.sk + ")");
                ok = b.xfer(rq, r, 3000);
            } else { if (!quiet) out("[LOCKED] " + why); b.lastNrc = 0x33; }
        }
        if (!ok) {
            if (b.lastNrc) out("[REFUSED] " + nrcText(b.lastNrc) + (b.lastNrc != 0x33 ? "" : (m.sfd || pack.sfd) && m.sk.empty() ? " - on this vehicle line coding needs the online authentication (SFD)" :
                                                                " - needs security access (no coding key available for this module)"));
            else out("[NO ANSWER] module did not respond");
            return false;
        }
        if (!quiet) out("< " + hexStr(r));
        if (!pack.decode(s, r, vals, err)) { out("[DECODE] " + err); return false; }
        return true;
    }

    // What a routine status says: 1 still running, 2 finished, 3 failed / aborted, 0 cannot tell.
    // The first value that says something decides (the routine's own status comes first in the response);
    // condition flags and step bits further down are not read as the status.
    static int textState(const std::string &raw) {
        std::string t = lowerAscii(raw);
        if (t.empty()) return 0;
        if (hasAny(t, {"not started", "nicht gestartet", "keine dia anfrage", "no request"})) return 0;
        if (hasAny(t, {"abort", "abgebroch", "abbruch", "fehlerhaft", "fehler gespeichert", "error", "failed", "fehlgeschlagen", "nicht erfolgreich",
                       "not successful", "unsuccessful", "n.i.o", "abgelehnt", "rejected", "denied", "canceled", "cancelled",
                       "nicht durchf", "not possible", "not feasible", "cannot be carried"})) return 3;
        if (hasAny(t, {"abgelaufen", "beendet", "finished", "completed", "complete", "abgeschlossen", "erfolgreich", "successful", "done",
                       "fertig", "i.o", "correctly", "expired", "ended", "terminated"})) return 2;
        if (hasAny(t, {"not active", "nicht aktiv", "inactive", "inaktiv", "not running"})) return 2;
        if (hasAny(t, {"active", "aktiv", "running", "l\xC3\xA4uft", "laeuft", "in progress", "busy", "in arbeit", "gestartet", "started",
                       "akzeptiert", "accepted", "pending", "wartet", "waiting"})) return 1;
        return 0;
    }
    static int routineState(const std::vector<pk::Value> &vs) {
        for (auto &v : vs) {
            if (v.text.empty()) continue;
            if (hasAny(lowerAscii(v.text), {"bedingung", "condition", "zwischenschritt", "intermediate step"})) continue;
            int st = textState(v.text);
            if (st) return st;
        }
        return 0;
    }
    // Conditions a routine reports as not met (e.g. "selector lever in P: condition not met").
    static std::string unmetConditions(const std::vector<pk::Value> &vs) {
        std::string o;
        for (auto &v : vs) {
            std::string t = lowerAscii(v.text);
            if (hasAny(t, {"nicht erf\xC3\xBCllt", "nicht erfuellt", "not met", "not fulfilled", "not fullfilled", "not satisfied"}))
                o += "\n    - " + v.name;
        }
        return o;
    }

    int readbackOf(const pk::Module &m, int writeSvc) const {
        double key = -1; if (!constAt(m.svc[writeSvc], 1, key)) return -1;
        for (int i = 0; i < (int)m.svc.size(); i++) {
            double k2 = -1; int sid = sidOf(m.svc[i]);
            if ((sid == 0x22 || sid == 0x21) && constAt(m.svc[i], 1, k2) && k2 == key) return i;
        }
        return -1;
    }

    // Run one function end to end (the module must be connected). guided = part of a job: a routine is
    // followed until its status says it is over, without waiting for STOP.
    bool runEntry(Bus &b, const pk::Module &m, const Entry &e, int proto, const pk::Inputs &inputs, bool guided = false) {
        std::vector<pk::Value> vals;
        out("=== " + e.name + " ===");
        if (e.cat == 'W') {                                         // show the current value first
            int rb = readbackOf(m, e.main);
            if (rb >= 0) { out("Current value:"); if (runService(b, m, rb, proto, {}, vals, true)) printValues(vals); }
        }
        if (!runService(b, m, e.main, proto, inputs, vals)) return false;
        printValues(vals);
        bool ok = true;
        if (e.cat == 'R' && e.result >= 0 && e.result != e.main) {   // follow the routine
            out(guided ? "Routine started - following the status..." : "Routine started. Status follows every second - press STOP to end the routine.");
            std::string last; uint32_t t0 = nowMs(), tp = t0, tchg = t0; int state = 0, sameState = 0;
            std::vector<pk::Value> lastVals;
            uint32_t limit = autoMode ? 5000u : guided ? 180000u : 600000u;
            bool natural = false;
            while (!stop && nowMs() - t0 < limit) {
                nap(1000);
                std::vector<pk::Value> rv;
                if (runService(b, m, e.result, proto, {}, rv, true)) {
                    lastVals = rv;
                    std::string t; for (auto &v : rv) {
                        std::string tl = lowerAscii(v.text);       // condition / step flags are summarised below, not listed every second
                        if (hasAny(tl, {"bedingung", "condition", "zwischenschritt", "intermediate step"})) continue;
                        if (v.text.empty() && v.unit.empty()) continue;            // structure header
                        t += v.name + " = " + v.text + (v.unit.empty() ? "" : " " + v.unit) + "; ";
                    }
                    if (t != last) { char ts[16]; snprintf(ts, sizeof ts, "[%3us] ", (unsigned)((nowMs() - t0) / 1000)); out(ts + t); last = t; tchg = nowMs(); }
                    int st = routineState(rv);
                    sameState = st == state ? sameState + 1 : 1; state = st;
                    if ((state == 2 || state == 3) && sameState >= 2) { natural = true; break; }
                    if (guided && state == 0 && (quick || nowMs() - tchg > 8000) && sameState >= 3) { natural = true; break; }
                }
                if (nowMs() - tp > 2000) { testerPresent(b, proto); tp = nowMs(); }
            }
            if (state == 3) {
                out("[FAILED] the control unit reports the routine did not complete: " + last); ok = false;
                std::string u = unmetConditions(lastVals);
                if (!u.empty()) out("  Conditions not met:" + u);
            }
            else if (natural) out(state == 2 ? "[OK] routine finished" : "[DONE] status no longer changes: " + last);
            else if (!stop && !autoMode) { out("[TIMEOUT] the routine did not report an end"); ok = false; }
            if (!natural && e.stop >= 0) { out("Stopping routine..."); runService(b, m, e.stop, proto, {}, vals, true); }
        } else if (e.cat == 'A') {                                  // hold the actuator until STOP
            if (e.ret >= 0 || e.stop >= 0) {
                out("Actuator active - press STOP to hand control back to the module.");
                uint32_t tp = nowMs(), t0 = tp;
                while (!stop && !((autoMode || guided) && nowMs() - t0 > 3000)) { nap(200); if (nowMs() - tp > 2000) { testerPresent(b, proto); tp = nowMs(); } }
                int back = e.ret >= 0 ? e.ret : e.stop;
                if (runService(b, m, back, proto, {}, vals, true)) out("[OK] control returned to the module");
            }
        } else if (e.cat == 'W') {
            out("[OK] written. Value now:");
            int rb = readbackOf(m, e.main);
            if (rb >= 0 && runService(b, m, rb, proto, {}, vals, true)) printValues(vals);
        }
        return ok;
    }
    bool runFunction(Bus &b, int mi, int entry, const pk::Inputs &inputs) {
        if (mi < 0 || mi >= (int)pack.mods.size()) return false;
        if (mi != curMod) selectModule(mi);
        if (entry < 0 || entry >= (int)entries.size()) return false;
        const pk::Module &m = pack.mods[mi]; int proto = m.proto;
        if (!connectModule(b, m, proto)) { out("Module does not answer (ignition on? fitted?)."); return false; }
        return runEntry(b, m, entries[entry], proto, inputs);
    }
    // Live data: repeat one or several read functions until STOP. Numeric values also go to onLive (graph).
    void runLive(Bus &b, int mi, const std::vector<int> &sel, const pk::Inputs &inputs, int maxRounds = 0) {
        if (mi < 0 || mi >= (int)pack.mods.size() || sel.empty()) return;
        if (mi != curMod) selectModule(mi);
        const pk::Module &m = pack.mods[mi]; int proto = m.proto;
        if (!connectModule(b, m, proto)) { out("Module does not answer (ignition on? fitted?)."); return; }
        uint32_t tp = nowMs(); int n = 0;
        while (!stop && (!maxRounds || n < maxRounds)) {
            std::string t = "Live data   (update " + std::to_string(++n) + ", STOP to end)\r\n"; bool any = false;
            for (int ei : sel) {
                if (ei < 0 || ei >= (int)entries.size() || stop) continue;
                const Entry &e = entries[ei]; std::vector<pk::Value> vals;
                t += "\r\n" + e.name + "\r\n";
                if (runService(b, m, e.main, proto, sel.size() == 1 ? inputs : pk::Inputs(), vals, true)) {
                    any = true;
                    for (auto &v : vals) {
                        std::string ind((size_t)std::max(1, v.depth) * 2, ' ');
                        t += ind + v.name + (v.text.empty() && v.unit.empty() ? ":" : " = " + v.text + (v.unit.empty() ? "" : " " + v.unit)) + "\r\n";
                    }
                    if (onLive) onLive(e.name, vals);
                } else t += "  (no answer)\r\n";
            }
            setOut(t);
            if (!any) nap(500);
            if (nowMs() - tp > 2000) { testerPresent(b, proto); tp = nowMs(); }
            nap(150);
        }
    }

    // ------------------------------------------------------------ guided jobs
    const pk::Job *findJob(const std::string &id) const { for (auto &j : pack.jobs) if (j.id == id) return &j; return nullptr; }
    std::string jobNote(const pk::Job &j) const {
        std::string mods, fn;
        for (auto &s : j.steps) { const pk::Module &m = pack.mods[s.mod]; mods += m.key + " " + modName(m) + " "; fn += pack.S(m.svc[s.svc].name) + " "; }
        std::string n = safetyNote('J', mods, j.title, fn);
        return j.warn.empty() ? n : j.warn + "\n\n" + n;
    }
    // Input values of one job step: fixed values and today's date are filled in, the values the user has
    // to choose are taken from 'asked' in order (missing ones become 0).
    static pk::Inputs stepInputs(const pk::JobStep &st, const pk::Inputs &asked) {
        time_t t = time(nullptr); struct tm tmv;
#ifdef _WIN32
        tmv = *localtime(&t);
#else
        localtime_r(&t, &tmv);
#endif
        pk::Inputs v; size_t a = 0;
        for (size_t k = 0; k < st.in.size(); k++) {
            int kind = k < st.kind.size() ? st.kind[k] : pk::IN_FIXED;
            if (kind == pk::IN_ASK) { v.add(a < asked.size() ? asked[a] : 0, a < asked.txt.size() ? asked.txt[a] : ""); a++; }
            else v.add(kind == pk::IN_YEAR ? tmv.tm_year + 1900 : kind == pk::IN_YEAR2 ? tmv.tm_year % 100 : kind == pk::IN_MONTH ? tmv.tm_mon + 1 :
                       kind == pk::IN_DAY ? tmv.tm_mday : kind == pk::IN_YMD ? ((tmv.tm_year % 100) << 16 | (tmv.tm_mon + 1) << 8 | tmv.tm_mday) : st.in[k]);
        }
        return v;
    }
    static int askCount(const pk::JobStep &st) { int n = 0; for (int k : st.kind) if (k == pk::IN_ASK) n++; return n; }
    // Runs every step of a job. asked[i] are the values the user chose for step i (see stepInputs).
    bool runJob(Bus &b, const pk::Job &j, const std::vector<pk::Inputs> &inputs) {
        out("=== JOB: " + j.title + " ===");
        int n = 0;
        for (auto &st : j.steps) {
            n++;
            if (stop) { out("[STOPPED] job stopped before step " + std::to_string(n)); return false; }
            const pk::Module &m = pack.mods[st.mod];
            std::vector<Entry> es = buildEntries(m); const Entry *e = nullptr;
            for (auto &x : es) if (x.main == st.svc || x.result == st.svc) { e = &x; break; }
            out("Step " + std::to_string(n) + " of " + std::to_string(j.steps.size()) + ": " + (st.label.empty() ? (e ? e->name : "?") : st.label) + "   (" + modName(m) + ")");
            if (!e) { out("[JOB FAILED] function missing in this data pack"); return false; }
            int proto = m.proto;
            if (!connectModule(b, m, proto)) { out("[JOB FAILED] " + modName(m) + " does not answer (ignition on? fitted?)"); return false; }
            pk::Inputs in = stepInputs(st, n - 1 < (int)inputs.size() ? inputs[n - 1] : pk::Inputs());
            if (!runEntry(b, m, *e, proto, in, true)) {
                out("[JOB FAILED] step " + std::to_string(n) + " did not complete. Nothing further was done. Fix the cause shown above and run the job again.");
                return false;
            }
        }
        out("[JOB DONE] " + j.title + " completed" + (j.steps.size() > 1 ? " - all " + std::to_string(j.steps.size()) + " steps" : "") + ".");
        return true;
    }
};

} // namespace pt
