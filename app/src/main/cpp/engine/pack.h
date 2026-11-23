// OWIS data pack: loading, request building and response decoding (ODX semantics).
// Pure C++ (no Windows) so it is unit-tested on any OS against real packs.
#pragma once
#include "json.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace pk {

enum Base { B_INT = 0, B_UINT = 1, B_F32 = 2, B_F64 = 3, B_ASCII = 4, B_UTF8 = 5, B_UNI = 6, B_BYTES = 7 };
enum Cat { C_IDENT = 0, C_LINEAR = 1, C_SCALE = 2, C_TEXT = 3 };
enum Sess { S_DEFAULT = 1, S_EXTENDED = 2, S_EOL = 4, S_DEV = 8, S_PROG = 16 };

struct Scale { bool hasLo = false, hasHi = false; double lo = 0, hi = 0; bool lin = false; double n0 = 0, n1 = 1, den = 1; int text = -1; };
struct Field { int name; int byte, bit; int dop; bool hasConst; double cval; int bits; };
struct Dop {
    char t = 's';                     // 's' simple, 't' struct, 'f' repeated field
    int base = B_UINT, bits = 8, lentype = 0; bool msb = true; int cat = C_IDENT; std::vector<Scale> sc; int unit = -1;
    std::vector<uint8_t> mask;
    std::vector<Field> f; int item = -1;
};
struct Param { int name; int byte, bit, bits; bool hasConst; double cval; int dop; };
struct Service { int name; std::string cls, sem; int sess; std::vector<Param> rq, rs; };
struct Dtc { uint32_t code; int bits; int text; };
struct Module {
    std::string key; int name; std::vector<std::pair<int, int>> ids; int proto; std::vector<Service> svc; std::vector<Dtc> dtc;
    std::string sk;                       // coding seed/key algorithm name (reference only), "" = none mapped
    int saLevel = 0, saBytes = 0;         // SecurityAccess request-seed sub-function for coding and the seed length (0 = unknown)
    bool sfd = false;                     // protected by online authentication (SFD): coding cannot be unlocked offline
    int link = 0;                         // 0 = diagnostic CAN (ISO-TP), 1 = K-line, 2 = VW TP 2.0 (ids[0].first = ECU address)
    bool addrConfirmed = true;            // false: address from the VW convention, not from the database
    int klInit = 2, kl5Baud = 0, klBaud = 10400;      // K-line: init type (1 five-baud, 2 fast), 5-baud address, baud rate
};
// One guided job: a short list of functions run in order, resolved when the pack is built.
// in[k] / kind[k] describe input k of the step's function: IN_FIXED uses in[k], IN_ASK is chosen by the
// user, IN_YEAR.. are filled with today's date when the job runs.
enum InKind { IN_FIXED = 0, IN_ASK = 1, IN_YEAR = 2, IN_YEAR2 = 3, IN_MONTH = 4, IN_DAY = 5, IN_YMD = 6 };   // IN_YMD: 3 bytes yy mm dd
struct JobStep { int mod = -1, svc = -1; bool ask = false; std::vector<double> in; std::vector<int> kind; std::string label; };
struct Job { std::string id, title, desc, warn; std::vector<JobStep> steps; };

struct Value { std::string name, text, unit; double num = NAN, raw = NAN; int depth = 0; };

// Input values for a request, in the order inputsOf() lists them. Numbers are enough for most functions;
// text and byte-string fields (part numbers, serial numbers, dealer codes) take txt[k] instead: plain text
// for character fields, hex bytes ("0A 1B 2C") for byte fields.
struct Inputs : std::vector<double> {
    std::vector<std::string> txt;
    Inputs() {}
    Inputs(std::initializer_list<double> l) : std::vector<double>(l) {}
    Inputs(const std::vector<double> &v) : std::vector<double>(v) {}
    void add(double n, const std::string &t = "") { txt.resize(size()); push_back(n); txt.push_back(t); }
};

class Pack {
public:
    std::string platform, title, lang;
    std::vector<std::string> str;
    std::vector<Dop> dops;
    std::vector<Module> mods;
    std::vector<std::string> vin;         // VIN model types this pack covers ("970", "991", ...)
    bool sfd = false;                     // the line uses online authentication (SFD) for coding
    std::vector<Job> jobs;

    const std::string &S(int i) const { static std::string z; return i >= 0 && i < (int)str.size() ? str[i] : z; }

    bool load(const std::string &text, std::string &err) {
        JVal j; JParser jp;
        if (!jp.parse(text, j) || j.t != JVal::OBJ) { err = "not a valid data pack (JSON)"; return false; }
        if (j["v"].i() != 1) { err = "unsupported pack version"; return false; }
        platform = j["platform"].s; title = j["title"].s; lang = j["lang"].s;
        for (auto &s : j["str"].a) str.push_back(s.s);
        for (auto &d : j["dops"].a) dops.push_back(dopOf(d));
        for (auto &m : j["mods"].a) {
            Module mo; mo.key = m["k"].s; mo.name = m["n"].i(); mo.proto = m["p"].i();
            if (!m["sk"].isNull()) mo.sk = m["sk"].s;
            if (m["sa"].t == JVal::ARR) { mo.saLevel = m["sa"][0].i(); mo.saBytes = m["sa"][1].i(); }
            if (!m["sfd"].isNull()) mo.sfd = m["sfd"].i() != 0;
            if (!m["tpc"].isNull()) mo.addrConfirmed = m["tpc"].i() != 0;
            if (!m["link"].isNull()) mo.link = m["link"].i();
            if (m["kl"].t == JVal::ARR) { mo.klInit = m["kl"][0].i(); mo.kl5Baud = m["kl"][1].i(); mo.klBaud = m["kl"][2].i() > 0 ? m["kl"][2].i() : 10400; }
            for (auto &p : m["ids"].a) mo.ids.push_back({p[0].i(), p[1].i()});
            for (auto &s : m["svc"].a) {
                Service sv; sv.name = s[0].i(); sv.cls = s[1].s; sv.sem = s[2].s; sv.sess = s[3].i();
                sv.rq = params(s[4]); sv.rs = params(s[5]); mo.svc.push_back(std::move(sv));
            }
            for (auto &d : m["dtc"].a) mo.dtc.push_back({(uint32_t)d[0].ll(), d[1].i(), d[2].i()});
            mods.push_back(std::move(mo));
        }
        for (auto &v : j["vin"].a) vin.push_back(v.s);
        sfd = j["sfd"].i() != 0;
        for (auto &jb : j["jobs"].a) {
            Job o; o.id = jb["id"].s; o.title = jb["t"].s; o.desc = jb["d"].s; o.warn = jb["w"].s;
            for (auto &st : jb["steps"].a) {
                JobStep js; js.mod = st[0].i(); js.svc = st[1].i(); js.ask = st[2].i() != 0;
                for (auto &x : st[3].a) {
                    int k = x.isNull() ? IN_ASK : x.t != JVal::STR ? IN_FIXED : x.s == "$Y" ? IN_YEAR : x.s == "$y" ? IN_YEAR2 : x.s == "$M" ? IN_MONTH : x.s == "$D" ? IN_DAY : x.s == "$ymd" ? IN_YMD : IN_ASK;
                    js.in.push_back(k == IN_FIXED ? x.n : 0); js.kind.push_back(k);
                }
                js.label = st[4].s;
                if (js.mod >= 0 && js.mod < (int)mods.size() && js.svc >= 0 && js.svc < (int)mods[js.mod].svc.size()) o.steps.push_back(js);
            }
            if (!o.steps.empty() && o.steps.size() == jb["steps"].a.size()) jobs.push_back(std::move(o));
        }
        return true;
    }

    // ------------------------------------------------------------------ request
    // Builds the request bytes. 'inputs' supplies physical values for the variable request fields in order
    // (texttable: the raw key is taken from the matching text, or the number itself).
    bool buildRequest(const Service &s, const Inputs &inputs, std::vector<uint8_t> &out, std::string &err) const {
        out.clear(); size_t in = 0;
        for (const Param &p : s.rq) {
            if (p.hasConst) {
                if (p.bits > 64) { size_t end = p.byte + (size_t)(p.bit + p.bits + 7) / 8; if (out.size() < end) out.resize(end, 0); }
                else putBits(out, p.byte, p.bit, p.bits ? p.bits : 8, (uint64_t)p.cval, true);
                continue;
            }
            if (p.dop >= 0) {
                if (!encode(p.dop, p.byte, p.bit, inputs, in, out, err)) return false;
            }
        }
        return true;
    }
    // Variable request inputs (name, dop) in the order buildRequest consumes them.
    void inputsOf(const Service &s, std::vector<std::pair<int, int>> &out) const {
        out.clear();
        for (const Param &p : s.rq) if (!p.hasConst && p.dop >= 0) collectInputs(p.dop, p.name, out);
    }

    // ------------------------------------------------------------------ response
    // Decodes a positive response. Constant parameters (service id, identifier) are checked, not shown.
    bool decode(const Service &s, const std::vector<uint8_t> &r, std::vector<Value> &out, std::string &err) const {
        out.clear();
        for (const Param &p : s.rs) {
            if (p.hasConst) {
                // Only the leading identifier bytes (service id, sub-function, data identifier) are checked.
                // Reserved bits further back are not: control units do not always send them as documented.
                if (p.bits > 64 || p.bit != 0 || (p.bits && p.bits % 8) || p.byte > 3) continue;
                uint64_t v; size_t used;
                if (!getBits(r, p.byte, p.bit, p.bits ? p.bits : 8, true, v, used)) { err = "response too short"; return false; }
                if ((double)v != p.cval) { err = "unexpected response (identifier mismatch)"; return false; }
                continue;
            }
            if (p.dop >= 0) decodeDop(p.dop, p.name, r, p.byte, p.bit, 0, out);
        }
        return true;
    }

    // Tolerant decoding for fault environment data: decodes the parameters in order and stops at the first
    // one the response does not contain (short answer) or whose constant does not match (other layout).
    // Returns the number of response bytes covered by what was decoded.
    size_t decodePartial(const Service &s, const std::vector<uint8_t> &r, std::vector<Value> &out) const {
        out.clear(); size_t covered = 0;
        for (const Param &p : s.rs) {
            if (p.hasConst) {
                uint64_t v; size_t used;
                if (!getBits(r, p.byte, p.bit, p.bits ? p.bits : 8, true, v, used) || (double)v != p.cval) break;
                if (p.byte + used > covered) covered = p.byte + used;
                continue;
            }
            if (p.dop < 0 || p.dop >= (int)dops.size()) continue;
            size_t before = out.size();
            decodeDop(p.dop, p.name, r, p.byte, p.bit, 0, out);
            bool miss = false; for (size_t k = before; k < out.size(); k++) if (out[k].text == "(missing)") miss = true;
            if (miss) { out.resize(before); break; }
            const Dop &pd = dops[p.dop];
            size_t end = p.byte + (pd.t == 's' && pd.lentype == 0 ? (size_t)(p.bit + pd.bits + 7) / 8 : (size_t)std::max(1, sizeOf(p.dop)));
            if (end > covered) covered = end;
        }
        return covered;
    }

    // true for inputs that are entered as text / hex bytes instead of a number
    // A byte field that has a value table (selection) is a number in disguise: encoded / decoded as unsigned.
    static bool numericBytes(const Dop &d) { return d.base == B_BYTES && d.cat == C_TEXT && d.lentype == 0 && d.bits > 0 && d.bits <= 64; }
    static bool isTextual(const Dop &d) { return d.t == 's' && !numericBytes(d) && (d.base >= B_ASCII || d.bits > 64 || d.lentype != 0); }

    // value -> text (used by decodeDop and by the UI for inputs)
    std::string textOf(const Dop &d, double raw, double &phys, bool &isNum) const {
        isNum = false; phys = raw;
        if (d.cat == C_TEXT) {
            for (auto &s : d.sc) if (inRange(s, raw) && s.text >= 0) return S(s.text);
            char b[32]; snprintf(b, sizeof b, "%.0f", raw); return b;
        }
        if (d.cat == C_LINEAR || d.cat == C_SCALE) {
            for (auto &s : d.sc) {
                if (d.cat == C_SCALE && !inRange(s, raw)) continue;
                if (s.text >= 0 && !s.lin) return S(s.text);
                if (s.lin) { phys = (s.n0 + s.n1 * raw) / (s.den ? s.den : 1); break; }
            }
        }
        isNum = true; return fmtNum(phys);
    }
    static std::string fmtNum(double v) {
        char b[48];
        if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) snprintf(b, sizeof b, "%.0f", v);
        else snprintf(b, sizeof b, "%.4g", v);
        return b;
    }

private:
    static bool inRange(const Scale &s, double raw) {
        if (s.hasLo && raw < s.lo) return false;
        if (s.hasHi && raw > s.hi) return false;
        return true;
    }
    Dop dopOf(const JVal &d) const {
        Dop o;
        if (d[0].s == "s") {
            o.t = 's'; o.base = d[1].i(); o.bits = d[2].i(); o.lentype = d[3].i(); o.msb = d[4].i() != 0; o.cat = d[5].i();
            for (auto &s : d[6].a) {
                Scale c;
                if (!s[0].isNull()) { c.hasLo = true; c.lo = s[0].n; }
                if (!s[1].isNull()) { c.hasHi = true; c.hi = s[1].n; }
                if (!s[3].isNull()) { c.lin = true; c.n0 = s[2].isNull() ? 0 : s[2].n; c.n1 = s[3].n; c.den = s[4].isNull() ? 1 : s[4].n; }
                c.text = s[5].i();
                o.sc.push_back(c);
            }
            o.unit = d[7].i();
            if (d[8].t == JVal::STR) for (size_t k = 0; k + 1 < d[8].s.size(); k += 2) o.mask.push_back((uint8_t)strtoul(d[8].s.substr(k, 2).c_str(), nullptr, 16));
        } else if (d[0].s == "t") {
            o.t = 't';
            for (auto &f : d[1].a) o.f.push_back({f[0].i(), f[1].i(), f[2].i(), f[3].i(), !f[4].isNull(), f[4].isNull() ? 0 : f[4].n,
                                                  f[5].isNull() || f[5].i() <= 0 ? 8 : f[5].i()});
        } else if (d[0].s == "f") { o.t = 'f'; o.item = d[1].i(); }
        return o;
    }
    std::vector<Param> params(const JVal &a) const {
        std::vector<Param> v;
        for (auto &p : a.a) v.push_back({p[0].i(), p[1].i(), p[2].i(), p[3].i(), !p[4].isNull(), p[4].isNull() ? 0 : p[4].n, p[5].i()});
        return v;
    }

    // ODX bit extraction: BYTE-POSITION + BIT-POSITION (from the least significant bit) + BIT-LENGTH.
    static bool getBits(const std::vector<uint8_t> &r, size_t byte, int bit, int bits, bool msb, uint64_t &v, size_t &used) {
        if (bits <= 0 || bits > 64) return false;
        size_t n = (size_t)(bit + bits + 7) / 8;
        if (n > 8 || byte + n > r.size()) return false;
        uint64_t acc = 0;
        for (size_t k = 0; k < n; k++) acc = (acc << 8) | r[byte + (msb ? k : n - 1 - k)];
        acc >>= bit;
        v = bits == 64 ? acc : (acc & ((1ULL << bits) - 1));
        used = n; return true;
    }
    static void putBits(std::vector<uint8_t> &o, size_t byte, int bit, int bits, uint64_t v, bool msb) {
        size_t n = (size_t)(bit + bits + 7) / 8;
        if (n > 8) n = 8;
        if (o.size() < byte + n) o.resize(byte + n, 0);
        uint64_t acc = 0;
        for (size_t k = 0; k < n; k++) acc = (acc << 8) | o[byte + (msb ? k : n - 1 - k)];
        uint64_t m = (bits >= 64 ? ~0ULL : ((1ULL << bits) - 1)) << bit;
        acc = (acc & ~m) | ((v << bit) & m);
        for (size_t k = 0; k < n; k++) o[byte + (msb ? n - 1 - k : k)] = (uint8_t)(acc >> (8 * k));
    }

    // byte length of a structure (for repeated fields)
    int sizeOf(int di, int depth = 0) const {
        if (di < 0 || di >= (int)dops.size() || depth > 8) return 0;
        const Dop &d = dops[di];
        if (d.t == 's') return d.lentype == 0 ? (d.bits + 7) / 8 : 0;
        if (d.t == 't') { int m = 0; for (auto &f : d.f) { int e = f.byte + (f.dop >= 0 ? sizeOf(f.dop, depth + 1) : 1); if (e > m) m = e; } return m; }
        return 0;
    }

    void decodeDop(int di, int name, const std::vector<uint8_t> &r, size_t byte, int bit, int depth, std::vector<Value> &out) const {
        if (di < 0 || di >= (int)dops.size() || depth > 8) return;
        const Dop &d = dops[di];
        if (d.t == 't') {
            if (depth > 0 && name >= 0 && d.f.size() > 1) { Value h; h.name = S(name); h.depth = depth; out.push_back(h); }
            for (auto &f : d.f) {
                if (f.hasConst || f.dop < 0) continue;
                decodeDop(f.dop, f.name, r, byte + f.byte, f.bit, depth + 1, out);
            }
            return;
        }
        if (d.t == 'f') {
            int sz = sizeOf(d.item); if (sz <= 0) return;
            for (size_t b = byte, k = 1; b + sz <= r.size() && k <= 255; b += sz, k++) {
                Value h; char t[24]; snprintf(t, sizeof t, " #%d", (int)k); h.name = S(name) + t; h.depth = depth; out.push_back(h);
                decodeDop(d.item, -1, r, b, 0, depth + 1, out);
            }
            return;
        }
        Value v; v.name = S(name); v.depth = depth; v.unit = S(d.unit);
        if (d.base == B_ASCII || d.base == B_UTF8 || d.base == B_UNI || (d.base == B_BYTES && !numericBytes(d))) {
            size_t n = d.lentype == 0 ? (size_t)(d.bits / 8) : (byte < r.size() ? r.size() - byte : 0);
            if (byte + n > r.size()) { v.text = "(missing)"; out.push_back(v); return; }
            if (d.base == B_BYTES) { char b[4]; for (size_t k = 0; k < n; k++) { snprintf(b, sizeof b, "%02X", r[byte + k]); v.text += (k ? " " : ""); v.text += b; } }
            else { for (size_t k = 0; k < n; k++) { uint8_t c = r[byte + k]; if (c >= 0x20 && c < 0x7F) v.text += (char)c; else if (c) v.text += ' '; }
                   while (!v.text.empty() && v.text.back() == ' ') v.text.pop_back(); }
            out.push_back(v); return;
        }
        uint64_t raw; size_t used;
        if (!getBits(r, byte, bit, d.bits, d.msb, raw, used)) { v.text = "(missing)"; out.push_back(v); return; }
        if (!d.mask.empty()) { uint64_t m = 0; for (uint8_t b : d.mask) m = (m << 8) | b; raw &= m; }
        double x;
        if (d.base == B_INT && d.bits < 64 && (raw >> (d.bits - 1)) & 1) x = (double)((int64_t)raw - ((int64_t)1 << d.bits));
        else if (d.base == B_F32 && d.bits == 32) { uint32_t u = (uint32_t)raw; float f; memcpy(&f, &u, 4); x = f; }
        else if (d.base == B_F64 && d.bits == 64) { double f; memcpy(&f, &raw, 8); x = f; }
        else x = (double)raw;
        bool isNum; double phys;
        v.text = textOf(d, x, phys, isNum); v.raw = x;
        if (isNum) v.num = phys; else v.unit.clear();
        out.push_back(v);
    }

    void collectInputs(int di, int name, std::vector<std::pair<int, int>> &out, int depth = 0) const {
        if (di < 0 || di >= (int)dops.size() || depth > 8) return;
        const Dop &d = dops[di];
        if (d.t == 't') { for (auto &f : d.f) if (!f.hasConst && f.dop >= 0) collectInputs(f.dop, f.name, out, depth + 1); return; }
        if (d.t == 's') out.push_back({name, di});
    }
    // A value is placed at its ODX byte and bit position; constants and reserved bits of a structure are
    // written with their own bit length so neighbouring fields in the same byte stay untouched.
    bool encode(int di, size_t byte, int bit, const Inputs &in, size_t &k, std::vector<uint8_t> &out, std::string &err, int depth = 0) const {
        if (di < 0 || di >= (int)dops.size() || depth > 8) { err = "unsupported input type"; return false; }
        const Dop &d = dops[di];
        if (d.t == 't') {
            for (auto &f : d.f) {
                if (f.hasConst) {
                    if (f.bits > 64) { size_t end = byte + f.byte + (size_t)(f.bit + f.bits + 7) / 8; if (out.size() < end) out.resize(end, 0); }   // long reserved area
                    else putBits(out, byte + f.byte, f.bit, f.bits > 0 ? f.bits : 8, (uint64_t)f.cval, true);
                    continue;
                }
                if (f.dop >= 0 && !encode(f.dop, byte + f.byte, f.bit, in, k, out, err, depth + 1)) return false;
            }
            return true;
        }
        if (d.t != 's') { err = "unsupported input type"; return false; }
        if (k >= in.size()) { err = "missing input value"; return false; }
        if (isTextual(d)) {                                  // character or byte-string field
            std::string t = k < in.txt.size() ? in.txt[k] : ""; k++;
            std::vector<uint8_t> v;
            if (d.base == B_ASCII || d.base == B_UTF8 || d.base == B_UNI) v.assign(t.begin(), t.end());
            else {
                std::string h; for (char c : t) if (c != ' ' && c != ':' && c != '-' && c != ',') h += c;
                if (h.size() % 2) { err = "byte field: enter pairs of hex digits, e.g. 0A 1B 2C"; return false; }
                for (size_t i = 0; i < h.size(); i += 2) {
                    if (!isxdigit((unsigned char)h[i]) || !isxdigit((unsigned char)h[i + 1])) { err = "byte field: enter pairs of hex digits, e.g. 0A 1B 2C"; return false; }
                    v.push_back((uint8_t)strtoul(h.substr(i, 2).c_str(), nullptr, 16));
                }
            }
            if (d.lentype == 0 && d.bits >= 8) {             // fixed length: text is padded with blanks, bytes with zeros
                size_t n = (size_t)d.bits / 8;
                if (v.size() > n) { err = "value too long: this field holds " + std::to_string(n) + (d.base == B_BYTES || d.base < B_ASCII ? " bytes" : " characters"); return false; }
                v.resize(n, d.base == B_ASCII || d.base == B_UTF8 ? 0x20 : 0x00);
            }
            if (out.size() < byte + v.size()) out.resize(byte + v.size(), 0);
            for (size_t i = 0; i < v.size(); i++) out[byte + i] = v[i];
            return true;
        }
        double phys = in[k++], raw = phys;
        if ((d.cat == C_LINEAR || d.cat == C_SCALE) && !d.sc.empty() && d.sc[0].lin && d.sc[0].n1 != 0)
            raw = (phys * d.sc[0].den - d.sc[0].n0) / d.sc[0].n1;
        long long iv = llround(raw);
        if (d.base == B_INT && iv < 0 && d.bits < 63) iv += (1LL << d.bits);
        if (d.bits <= 0 || d.bits > 64) { err = "input of variable length is not supported"; return false; }
        putBits(out, byte, bit, d.bits, (uint64_t)iv, d.msb);
        return true;
    }
};

} // namespace pk
