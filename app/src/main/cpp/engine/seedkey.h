// Codierung seed/key: loads the local key file (owis.keys) and computes the unlock key from a
// seed. The key file holds the unlock constants and is generated from a local PIWIS install
// (tools/build_keyfile.py); it is never shipped. Without it, coding writes are simply unavailable.
//
// Two algorithm families are supported here:
//   keyed-multiply  key = ((seed ^ K) * A + B) mod 2^(8*nb)       (4- or 2-byte seed)
//   rotate table    key = table[seed]                              (2-byte seed, 128 KB table)
// Array-hash algorithms are not in the key file yet (not ported).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>

namespace sk {

enum Fam { FAM_KMUL = 1, FAM_TABLE = 2, FAM_BMROT = 3, FAM_DIA = 4, FAM_LFSR = 5, FAM_KOMBI = 6, FAM_RSR = 7 };

struct Key {
    int fam = 0, nb = 4;
    uint32_t K = 0, A = 1, B = 0;      // keyed-multiply
    std::vector<uint8_t> table;        // rotate table: nb bytes per seed, indexed by seed value
    uint32_t rotleft = 0, rb[4] = {0}, cb[2] = {0};               // bmrot
    std::vector<uint8_t> init, xorm, fixed; int challenge = 0;    // dia (calc_bytes = init.size())
    uint32_t lconst = 0; int lrounds = 0;                         // lfsr
    uint32_t parm1 = 0, parmop = 0; int rrounds = 0;              // rsr
};

inline std::map<std::string, Key> &keys() { static std::map<std::string, Key> k; return k; }
inline bool &loaded() { static bool l = false; return l; }

// Load owis.keys. Returns the number of algorithms loaded (0 if the file is absent/invalid).
inline int load(const std::string &path) {
    keys().clear(); loaded() = false;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    std::vector<uint8_t> d; { fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        if (n > 0) { d.resize(n); size_t r = fread(d.data(), 1, n, f); d.resize(r); } }
    fclose(f);
    if (d.size() < 9 || memcmp(d.data(), "PTKEY1\0", 7) != 0) return 0;
    size_t p = 7;
    auto u16 = [&](size_t o) { return (uint16_t)(d[o] | (d[o + 1] << 8)); };
    auto u32 = [&](size_t o) { return (uint32_t)(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | ((uint32_t)d[o + 3] << 24)); };
    uint16_t count = u16(p); p += 2;
    for (int i = 0; i < count && p < d.size(); i++) {
        uint8_t nl = d[p++]; if (p + nl > d.size()) break;
        std::string name((const char *)&d[p], nl); p += nl;
        if (p + 2 > d.size()) break;
        Key k; k.fam = d[p++]; k.nb = d[p++];
        if (k.fam == FAM_KMUL) {
            if (p + 12 > d.size()) break;
            k.K = u32(p); k.A = u32(p + 4); k.B = u32(p + 8); p += 12;
        } else if (k.fam == FAM_TABLE) {
            if (p + 4 > d.size()) break;
            uint32_t tb = u32(p); p += 4; if (p + tb > d.size()) break;
            k.table.assign(d.begin() + p, d.begin() + p + tb); p += tb;
        } else if (k.fam == FAM_BMROT) {
            if (p + 28 > d.size()) break;
            k.rotleft = u32(p); for (int i = 0; i < 4; i++) k.rb[i] = u32(p + 4 + 4 * i);
            k.cb[0] = u32(p + 20); k.cb[1] = u32(p + 24); p += 28;
        } else if (k.fam == FAM_DIA) {
            auto rd = [&](std::vector<uint8_t> &v) {
                if (p >= d.size()) return false;
                uint8_t n = d[p++];
                if (p + n > d.size()) return false;
                v.assign(d.begin() + p, d.begin() + p + n); p += n; return true;
            };
            if (!rd(k.init) || !rd(k.xorm) || !rd(k.fixed)) break;
            if (p >= d.size()) break;
            k.challenge = d[p++];
        } else if (k.fam == FAM_LFSR) {
            if (p + 5 > d.size()) break;
            k.lconst = u32(p); k.lrounds = d[p + 4]; p += 5;
        } else if (k.fam == FAM_KOMBI) {
            // fixed algorithm, no parameters
        } else if (k.fam == FAM_RSR) {
            if (p + 9 > d.size()) break;
            k.parm1 = u32(p); k.parmop = u32(p + 4); k.rrounds = d[p + 8]; p += 9;
        } else break;
        keys()[name] = std::move(k);
    }
    loaded() = !keys().empty();
    return (int)keys().size();
}

inline const Key *find(const std::string &algo) {
    auto it = keys().find(algo); return it == keys().end() ? nullptr : &it->second;
}

// Compute the key from a seed. seed/out are big-endian byte vectors of length key.nb.
// Returns false if the algorithm cannot produce a key (e.g. table lookup out of range).
inline bool compute(const Key &k, const std::vector<uint8_t> &seed, std::vector<uint8_t> &out) {
    int nb = k.nb;
    uint64_t s = 0; for (int i = 0; i < nb && i < (int)seed.size(); i++) s = (s << 8) | seed[i];
    if (k.fam == FAM_KMUL) {
        uint64_t mask = (nb >= 8) ? ~0ULL : ((1ULL << (8 * nb)) - 1);
        uint64_t key = (((s ^ k.K) & mask) * k.A + k.B) & mask;
        out.assign(nb, 0); for (int i = 0; i < nb; i++) out[nb - 1 - i] = (uint8_t)(key >> (8 * i));
        return true;
    }
    if (k.fam == FAM_TABLE) {
        uint64_t idx = s * nb;
        if (idx + nb > k.table.size()) return false;
        out.assign(k.table.begin() + idx, k.table.begin() + idx + nb);
        return true;
    }
    if (k.fam == FAM_BMROT) {
        uint32_t v = (uint32_t)s;
        auto rol = [](uint32_t x, int n) { n &= 31; return n ? ((x << n) | (x >> (32 - n))) : x; };
        auto ror = [](uint32_t x, int n) { n &= 31; return n ? ((x >> n) | (x << (32 - n))) : x; };
        int rc = (v & k.rb[0] ? 1 : 0) + (v & k.rb[1] ? 2 : 0) + (v & k.rb[2] ? 4 : 0) + (v & k.rb[3] ? 8 : 0);
        int op = (v & k.cb[0] ? 1 : 0) + (v & k.cb[1] ? 2 : 0);
        uint32_t r = (v & k.rotleft) ? rol(v, rc) : ror(v, rc);
        uint32_t o = op == 0 ? (v & r) : op == 1 ? (v | r) : op == 2 ? (v ^ r) : ~v;
        out.assign(4, 0); for (int i = 0; i < 4; i++) out[3 - i] = (uint8_t)(o >> (8 * i));
        return true;
    }
    if (k.fam == FAM_DIA) {
        int cb = (int)k.init.size(); if (cb < 1 || k.xorm.size() != (size_t)cb) return false;
        int seed_used = k.challenge - (int)k.fixed.size();
        std::vector<uint8_t> msg;
        for (int i = 0; i < seed_used; i++) msg.push_back(i < (int)seed.size() ? seed[i] : 0);
        msg.insert(msg.end(), k.fixed.begin(), k.fixed.end());
        std::vector<uint8_t> c = k.init;
        for (uint8_t mb : msg) for (int bit = 0; bit < 8; bit++) {
            uint8_t t6 = (c[0] & 1) ? 0x80 : 0;
            for (int j = 0; j < cb - 1; j++) { c[j] = (c[j] >> 1) & 0x7F; if (c[j + 1] & 1) c[j] |= 0x80; }
            c[cb - 1] = (c[cb - 1] >> 1) & 0x7F; c[cb - 1] |= t6;
            if (mb & (1 << bit)) c[cb - 1] ^= 0x80;
            if (c[cb - 1] & 0x80) for (int j = 0; j < cb; j++) c[j] ^= k.xorm[j];
        }
        out.assign(4, 0);
        if (cb == 3) {  // 24-bit calc: pack 3 bytes into a 4-byte key (last byte 0)
            out[0] = ((c[1] & 0x0F) << 4) | ((c[0] & 0xF0) >> 4);
            out[1] = (c[1] & 0xF0) | ((c[2] & 0xF0) >> 4);
            out[2] = ((c[0] & 0x0F) << 4) | (c[2] & 0x0F);
            out[3] = 0;
        } else {
            out[0] = ((c[3] & 0x0F) << 4) | (c[3] & 0xF0);
            out[1] = ((c[1] & 0x0F) << 4) | ((c[0] & 0xF0) >> 4);
            out[2] = (c[1] & 0xF0) | ((c[2] & 0xF0) >> 4);
            out[3] = ((c[0] & 0x0F) << 4) | (c[2] & 0x0F);
        }
        return true;
    }
    if (k.fam == FAM_LFSR) {
        uint32_t v = (uint32_t)s;
        for (int i = 0; i < k.lrounds; i++) {
            uint32_t c = v & 0x80000000u; v = (v << 1) & 0xFFFFFFFEu;
            if (c) v = (v | 1u) ^ k.lconst;
        }
        out.assign(4, 0); for (int i = 0; i < 4; i++) out[3 - i] = (uint8_t)(v >> (8 * i));
        return true;
    }
    if (k.fam == FAM_KOMBI) {
        uint8_t sd[4]; for (int i = 0; i < 4; i++) sd[i] = i < (int)seed.size() ? seed[i] : 0;
        uint8_t o[4] = { (uint8_t)(sd[2] + 1), (uint8_t)(sd[0] + 2), (uint8_t)(sd[3] + 3), (uint8_t)(sd[1] + 4) };
        for (int j = 0; j < 31; j++) {
            int i = j % 4;
            o[i] = (uint8_t)(o[i] + sd[(j + 1) % 4]);
            o[i] = (uint8_t)(o[i] ^ sd[(j + 2) % 4]);
            o[(j + 3) % 4] = (uint8_t)(o[(j + 3) % 4] ^ sd[j % 4]);
        }
        out.assign(o, o + 4);
        return true;
    }
    if (k.fam == FAM_RSR) {
        uint32_t v = (uint32_t)s, c = 0;
        for (int i = 0; i < k.rrounds; i++) {
            v = (v + k.parm1) & 0xFFFFFFFFu;
            for (int t = 0; t < 2; t++) { c = v & 1u; v = (v >> 1) & 0x7FFFFFFFu; if (c) v |= 0x80000000u; }
            if (c) v ^= k.parmop;
        }
        out.assign(4, 0); for (int i = 0; i < 4; i++) out[3 - i] = (uint8_t)(v >> (8 * i));
        return true;
    }
    return false;
}

} // namespace sk
