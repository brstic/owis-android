// OWIS - protocol-independent core (no Windows, no J2534): response parsing,
// fault-code formatting and catalogue lookup.  Unit-tested on any OS by tests/test_core.cpp.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

enum Proto { PROTO_AUTO = 0, PROTO_UDS = 1, PROTO_KWP = 2 };

struct Module {          // one diagnostic address (variants of the same ECU are merged)
    int key; const char *name;
    uint16_t req, resp;   // primary CAN pair (0 = not reachable directly on the diagnostic CAN)
    uint16_t req2, resp2; // alternative pair (some variants), 0 = none
    int proto;
};
struct DtcText { int module; int bits; uint32_t code; const char *en; const char *de; };

struct Dtc {             // one fault as reported by the control unit
    uint32_t code;        // UDS: 24 bit, KWP: 16 bit
    int bits;             // 24 or 16
    uint8_t status;
};

// ---------------------------------------------------------------- parsing
// UDS 19 02 response: 59 02 <availMask> { DTC_hi DTC_mid DTC_lo status } ...
static inline bool parseUds1902(const std::vector<uint8_t> &r, std::vector<Dtc> &out) {
    out.clear();
    if (r.size() < 3 || r[0] != 0x59 || r[1] != 0x02) return false;
    for (size_t i = 3; i + 4 <= r.size(); i += 4)
        out.push_back({(uint32_t)((r[i] << 16) | (r[i+1] << 8) | r[i+2]), 24, r[i+3]});
    return true;
}
// KWP2000 18 xx response: 58 <count> { DTC_hi DTC_lo status } ...
static inline bool parseKwp18(const std::vector<uint8_t> &r, std::vector<Dtc> &out) {
    out.clear();
    if (r.size() < 2 || r[0] != 0x58) return false;
    for (size_t i = 2; i + 3 <= r.size(); i += 3)
        out.push_back({(uint32_t)((r[i] << 8) | r[i+1]), 16, r[i+2]});
    return true;
}

// ---------------------------------------------------------------- formatting
// 16-bit SAE code -> "P1872" (top two bits select P/C/B/U)
static inline std::string sae16(uint16_t c) {
    char b[8]; snprintf(b, sizeof b, "%c%04X", "PCBU"[c >> 14], c & 0x3FFF); return b;
}
static inline std::string dtcName(const Dtc &d) {
    char b[24];
    if (d.bits == 16) return sae16((uint16_t)d.code);
    snprintf(b, sizeof b, "%06X", d.code & 0xFFFFFF);
    return b;
}
// UDS status byte (ISO 14229 DTCStatusMask)
static inline std::string udsStatus(uint8_t s) {
    std::string t;
    if (s & 0x01) t += "active now";
    else if (s & 0x08) t += "stored";
    else if (s & 0x04) t += "pending";
    else t += "not active";
    if (s & 0x80) t += ", warning lamp";
    return t;
}
// KWP2000 (VW/Porsche style) status byte: bit 5/6 = present/stored, bit 7 = warning lamp
static inline std::string kwpStatus(uint8_t s) {
    std::string t = (s & 0x60) == 0x20 ? "stored" : (s & 0x40) ? "active now" : "stored";
    if (s & 0x80) t += ", warning lamp";
    return t;
}
static inline std::string statusText(const Dtc &d) { return d.bits == 24 ? udsStatus(d.status) : kwpStatus(d.status); }

// Full breakdown of the UDS status byte, for the detail view under a fault.
static inline std::string udsStatusDetail(uint8_t s) {
    std::string t;
    auto add = [&](const char *x) { if (!t.empty()) t += ", "; t += x; };
    if (s & 0x01) add("test failed now");
    if (s & 0x02) add("failed this drive cycle");
    if (s & 0x04) add("pending (not yet confirmed)");
    if (s & 0x08) add((s & 0x01) ? "confirmed" : "confirmed / stored");
    if ((s & 0x08) && !(s & 0x01) && !(s & 0x02)) add("not seen this drive cycle (ageing)");
    if (s & 0x20) add("failed since last clear");
    if (s & 0x10) add("not tested since last clear");
    if (s & 0x40) add("not tested this drive cycle");
    if (s & 0x80) add("warning lamp requested");
    if (t.empty()) t = "no status bits set";
    return t;
}

// ---------------------------------------------------------------- VIN
// A VIN is 17 characters, capital letters and digits without I, O, Q.
static inline bool vinValid(const std::string &v) {
    if (v.size() != 17) return false;
    for (char c : v) if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) || c == 'I' || c == 'O' || c == 'Q') return false;
    return true;
}
// Porsche model type from the VIN: positions 7, 8 and 12 (e.g. WP0ZZZ97ZCL0..... -> "970").
// North-American VINs describe the car in positions 4-6 and put a filler 'A' in position 7: the type is then
// '9' + position 8 + position 12 (WP0AA2A7XBL0..... -> "970").
static inline std::string vinModelType(const std::string &v) {
    if (!vinValid(v) || v.compare(0, 2, "WP") != 0) return "";
    if (v[6] == 'A' && v.compare(3, 3, "ZZZ") != 0) return std::string("9") + v[7] + v[11];
    return std::string() + v[6] + v[7] + v[11];
}
// Model year from position 10 (2001..2030 cycle; the older cycle is not used by the supported lines' packs).
static inline int vinModelYear(const std::string &v) {
    if (!vinValid(v)) return 0;
    static const char *Y = "123456789ABCDEFGHJKLMNPRSTVWXY";      // 2001 .. 2030
    const char *p = strchr(Y, v[9]); return p ? 2001 + (int)(p - Y) : 0;
}
// Hide the serial part of a VIN for reports that are shared (keeps maker, model and year).
static inline std::string vinRedact(const std::string &v) { return v.size() == 17 ? v.substr(0, 11) + "******" : v; }
// Pick a VIN out of a response payload (ASCII run of 17 valid characters).
static inline std::string vinFromBytes(const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 17 <= n; i++) {
        std::string v((const char *)p + i, 17);
        if (vinValid(v)) return v;
    }
    return "";
}

// Fault text lookup.  KWP codes are matched on 16 bit; UDS codes on 24 bit, falling back to the
// SAE part (top 16 bit) for ECUs that report "P-code + failure type byte".
static inline const DtcText *findText(const DtcText *tab, int module, const Dtc &d) {
    const DtcText *alt = nullptr;
    for (const DtcText *t = tab; t->module >= 0; t++) {
        if (t->module != module) continue;
        if (t->bits == d.bits && t->code == d.code) return t;
        if (d.bits == 24 && t->bits == 16 && t->code == (d.code >> 8)) alt = t;
    }
    return alt;
}

static inline std::string printable(const uint8_t *p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) s += (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    size_t a = s.find_first_not_of(' '); return a == std::string::npos ? "" : s.substr(a);
}
