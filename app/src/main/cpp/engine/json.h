// Minimal JSON reader (DOM) for OWIS data packs. No dependencies, any OS.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct JVal {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    double n = 0; bool b = false; std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;
    bool isNull() const { return t == NUL; }
    int i() const { return (int)n; }
    long long ll() const { return (long long)n; }
    size_t size() const { return t == ARR ? a.size() : t == OBJ ? o.size() : 0; }
    const JVal &operator[](int k) const { static JVal z; return t == ARR && k >= 0 && (size_t)k < a.size() ? a[k] : z; }
    const JVal &operator[](const char *k) const {
        static JVal z; if (t != OBJ) return z;
        for (auto &p : o) if (p.first == k) return p.second;
        return z;
    }
};

class JParser {
    const char *p, *e;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
    static void utf8(std::string &out, unsigned c) {
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 63)); out += (char)(0x80 | ((c >> 6) & 63)); out += (char)(0x80 | (c & 63)); }
    }
    bool str(std::string &out) {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                p++;
                switch (*p) {
                case 'n': out += '\n'; break; case 't': out += '\t'; break; case 'r': out += '\r'; break;
                case 'b': out += '\b'; break; case 'f': out += '\f'; break;
                case 'u': {
                    if (p + 4 >= e) return false;
                    unsigned c = (unsigned)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16); p += 4;
                    if (c >= 0xD800 && c < 0xDC00 && p + 6 < e && p[1] == '\\' && p[2] == 'u') {
                        unsigned d = (unsigned)strtoul(std::string(p + 3, 4).c_str(), nullptr, 16);
                        c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); p += 6;
                    }
                    utf8(out, c); break; }
                default: out += *p;
                }
                p++;
            } else out += *p++;
        }
        if (p >= e) return false;
        p++; return true;
    }
    bool val(JVal &v, int depth) {
        if (depth > 64) return false;
        ws(); if (p >= e) return false;
        char c = *p;
        if (c == '{') {
            v.t = JVal::OBJ; p++; ws();
            if (p < e && *p == '}') { p++; return true; }
            for (;;) {
                ws(); std::string k; if (!str(k)) return false; ws();
                if (p >= e || *p != ':') return false;
                p++;
                v.o.emplace_back(std::move(k), JVal());
                if (!val(v.o.back().second, depth + 1)) return false;
                ws(); if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.t = JVal::ARR; p++; ws();
            if (p < e && *p == ']') { p++; return true; }
            for (;;) {
                v.a.emplace_back();
                if (!val(v.a.back(), depth + 1)) return false;
                ws(); if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = JVal::STR; return str(v.s); }
        if (!strncmp(p, "true", 4)) { v.t = JVal::BOOL; v.b = true; p += 4; return true; }
        if (!strncmp(p, "false", 5)) { v.t = JVal::BOOL; p += 5; return true; }
        if (!strncmp(p, "null", 4)) { v.t = JVal::NUL; p += 4; return true; }
        char *end; v.n = strtod(p, &end);
        if (end == p) return false;
        v.t = JVal::NUM; p = end; return true;
    }
public:
    bool parse(const std::string &text, JVal &out) { p = text.data(); e = p + text.size(); return val(out, 0); }
};
