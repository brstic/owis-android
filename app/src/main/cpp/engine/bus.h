// OWIS transport abstraction: one diagnostic link to one control unit at a time.
// Portable (no Windows): the J2534 adapter, Linux SocketCAN, the simulated car and session replay all
// implement this interface, so everything above it (engine.h) is tested on any OS.
#pragma once
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace pt {

inline uint32_t nowMs() {
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0).count();
}
inline void sleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

inline std::string hexStr(const uint8_t *d, size_t n) {
    static const char *H = "0123456789ABCDEF"; std::string s;
    for (size_t i = 0; i < n; i++) { s += H[d[i] >> 4]; s += H[d[i] & 15]; if (i + 1 < n) s += ' '; }
    return s;
}
inline std::string hexStr(const std::vector<uint8_t> &v) { return hexStr(v.data(), v.size()); }

// How a control unit is reached.
enum LinkKind { LINK_CAN = 0,      // ISO 15765-2 on the diagnostic CAN, 500 kbit/s, 11- or 29-bit ids
                LINK_KLINE = 1,    // ISO 14230 (KWP2000) on the K-line, 10400 baud, fast init; tx = ECU address
                LINK_TP20 = 2 };   // VW TP 2.0 on the diagnostic CAN (units behind the gateway); tx = VW diagnostic address

class Bus {
public:
    uint32_t txId = 0, rxId = 0; int kind = LINK_CAN;
    int klInit = 2, kl5Baud = 0, klBaud = 10400;   // K-line: 1 = 5-baud init (address kl5Baud), 2 = fast init
    uint8_t lastNrc = 0; bool gotAnswer = false;
    bool fast = false;                       // simulated / replayed link: no real-time waiting needed
    std::function<void(char dir, uint32_t id, const std::vector<uint8_t> &)> tap;   // session recorder ('T' / 'R')
    std::function<void(const std::string &)> onError;

    virtual ~Bus() {}
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool sendRaw(const std::vector<uint8_t> &p) = 0;
    virtual bool recvRaw(std::vector<uint8_t> &out, unsigned timeoutMs) = 0;
    // Select the control unit to talk to. Returns false when the link cannot be set up at all (adapter
    // problem); a K-line unit that simply does not answer its initialisation is "no answer", not an error.
    virtual bool target(uint32_t tx, uint32_t rx, int linkKind = LINK_CAN) { txId = tx; rxId = rx; kind = linkKind; return true; }

    void error(const std::string &s) { if (onError) onError(s); }
    bool send(const std::vector<uint8_t> &p) { if (tap) tap('T', txId, p); return sendRaw(p); }
    bool recv(std::vector<uint8_t> &out, unsigned timeoutMs) {
        if (!recvRaw(out, timeoutMs)) return false;
        if (tap) tap('R', rxId, out);
        return true;
    }
    // Request -> positive response. Handles "response pending" (NRC 0x78); a negative response sets lastNrc.
    bool xfer(const std::vector<uint8_t> &p, std::vector<uint8_t> &out, unsigned tmo = 1500) {
        lastNrc = 0; gotAnswer = false; out.clear();
        if (p.empty() || !send(p)) return false;
        uint8_t want = (uint8_t)(p[0] + 0x40);
        uint32_t t0 = nowMs();
        while (nowMs() - t0 < tmo) {
            if (!recv(out, tmo)) return false;
            gotAnswer = true;
            if (!out.empty() && out[0] == want) return true;
            if (out.size() >= 3 && out[0] == 0x7F && out[1] == p[0]) {
                lastNrc = out[2];
                if (out[2] == 0x78) { t0 = nowMs(); tmo = 8000; continue; }
                return false;
            }
        }
        return false;
    }
};

inline std::string nrcText(uint8_t n) {
    switch (n) {
    case 0x10: return "general reject"; case 0x11: return "service not supported"; case 0x12: return "sub-function not supported";
    case 0x13: return "wrong length / format"; case 0x14: return "response too long"; case 0x21: return "busy, repeat request";
    case 0x22: return "conditions not correct"; case 0x24: return "request sequence error"; case 0x25: return "no response from sub-bus component";
    case 0x26: return "a fault prevents the action"; case 0x31: return "request out of range"; case 0x33: return "security access denied";
    case 0x35: return "invalid key"; case 0x36: return "too many unlock attempts"; case 0x37: return "unlock delay not yet expired";
    case 0x70: return "upload/download not accepted"; case 0x72: return "general programming failure";
    case 0x7E: case 0x7F: return "not supported in this session"; case 0x80: return "not supported in this session (KWP)";
    case 0x81: return "engine speed too high"; case 0x82: return "engine speed too low"; case 0x83: return "engine is running";
    case 0x84: return "engine is not running"; case 0x85: return "engine run time too low"; case 0x86: return "temperature too high";
    case 0x87: return "temperature too low"; case 0x88: return "vehicle speed too high"; case 0x89: return "vehicle speed too low";
    case 0x8A: return "throttle / pedal too high"; case 0x8B: return "throttle / pedal too low"; case 0x8C: return "transmission not in neutral";
    case 0x8D: return "transmission not in gear"; case 0x8F: return "brake not applied"; case 0x90: return "selector lever not in park";
    case 0x92: return "voltage too high"; case 0x93: return "voltage too low";
    default: { char b[16]; snprintf(b, sizeof b, "NRC 0x%02X", n); return b; } }
}

} // namespace pt
