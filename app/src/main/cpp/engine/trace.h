// Session record / replay. Every request and response of a session is written to a small text file; the
// same file can be replayed later as if the car were connected - for diagnosis after the car has left and
// for testing without a car.
//   # OWIS trace v1
//   @ <tx id hex> <rx id hex> <link>        a control unit was selected
//   T <hex bytes>                           request sent
//   R <hex bytes>                           response received
// A trace can contain the VIN and part numbers of the car: treat it like a scan report.
#pragma once
#include "bus.h"
#include <map>

namespace pt {

class TraceWriter {
    FILE *f = nullptr; uint32_t lastTx = 0xFFFFFFFFu, lastRx = 0; int lastKind = -1;
public:
    ~TraceWriter() { close(); }
    bool open(const std::string &path) { close(); f = fopen(path.c_str(), "a"); if (f) fprintf(f, "# OWIS trace v1\n"); lastTx = 0xFFFFFFFFu; return f != nullptr; }
    void close() { if (f) { fclose(f); f = nullptr; } }
    bool active() const { return f != nullptr; }
    void attach(Bus &b) {
        b.tap = [this, &b](char dir, uint32_t, const std::vector<uint8_t> &p) {
            if (!f) return;
            if (b.txId != lastTx || b.rxId != lastRx || b.kind != lastKind) {
                fprintf(f, "@ %X %X %d\n", (unsigned)b.txId, (unsigned)b.rxId, b.kind);
                lastTx = b.txId; lastRx = b.rxId; lastKind = b.kind;
            }
            fprintf(f, "%c %s\n", dir, hexStr(p).c_str()); fflush(f);
        };
    }
};

// Answers requests from a recorded trace. A request that was recorded several times is answered with the
// recorded answers in the same order (the last one repeats); an unknown request gets no answer.
class ReplayBus : public Bus {
    typedef std::vector<std::vector<uint8_t>> Answers;
    std::map<std::string, std::vector<Answers>> db; std::map<std::string, size_t> pos;
    std::vector<std::vector<uint8_t>> q; size_t qi = 0;
    static std::string key(uint32_t tx, const std::vector<uint8_t> &p) { char b[16]; snprintf(b, sizeof b, "%X:", (unsigned)tx); return b + hexStr(p); }
    static std::vector<uint8_t> bytes(const char *s) {
        std::vector<uint8_t> v;
        while (*s) { while (*s == ' ') s++; if (!s[0] || !s[1]) break; char h[3] = {s[0], s[1], 0}; v.push_back((uint8_t)strtoul(h, nullptr, 16)); s += 2; }
        return v;
    }
public:
    size_t requests = 0;
    ReplayBus() { fast = true; }
    bool load(const std::string &path) {
        FILE *f = fopen(path.c_str(), "r"); if (!f) return false;
        char line[16384]; uint32_t tx = 0; std::string cur;
        while (fgets(line, sizeof line, f)) {
            size_t n = strlen(line); while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (line[0] == '@') { unsigned a = 0, b = 0; sscanf(line + 1, "%x %x", &a, &b); tx = a; cur.clear(); }
            else if (line[0] == 'T') { cur = key(tx, bytes(line + 1)); db[cur].push_back(Answers()); requests++; }
            else if (line[0] == 'R' && !cur.empty()) db[cur].back().push_back(bytes(line + 1));
        }
        fclose(f);
        return requests > 0;
    }
    bool open() override { return !db.empty(); }
    void close() override {}
    bool sendRaw(const std::vector<uint8_t> &p) override {
        q.clear(); qi = 0;
        auto it = db.find(key(txId, p)); if (it == db.end()) return true;
        size_t &k = pos[it->first]; const Answers &a = it->second[k < it->second.size() ? k : it->second.size() - 1];
        if (k < it->second.size()) k++;
        q = a; return true;
    }
    bool recvRaw(std::vector<uint8_t> &out, unsigned) override {
        if (qi >= q.size()) return false;
        out = q[qi++]; return true;
    }
};

} // namespace pt
