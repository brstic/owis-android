// Simulated Panamera (970) for testing OWIS without a car. Portable: used by the native tests and
// the --demo mode through SimBus, and by the mock J2534 DLL (tests/mock_j2534.cpp) under Wine.
//
// Simulated control units (request/response CAN id):
//   0x710/0x77A Gateway (UDS)   : VIN (22 F190), no faults
//   0x714/0x77E Cluster (UDS)   : VIN, service-interval reset routine 31 01 0450
//   0x70F/0x779 PDK (KWP2000)   : P1872 stored; clear needs session 0x89; SecurityAccess 27 01/02 (2-byte seed)
//   0x755/0x7BF PASM (UDS)      : 000110 active + environment data (19 06); ride height 22/2E 00A0 (write needs
//                                 the extended session and SecurityAccess 27 03/04); levelling routine 31 xx F105
//   0x715/0x77F Airbag (UDS)    : no faults
//   0x753/0x7BD Selector (KWP)  : refuses UDS 0x19 (NRC 0x11) -> tests the UDS->KWP fallback; U0155 stored
//   0x7E0/0x7E8 Engine (KWP)    : sends "response pending" (0x78) once before answering
// and two K-line units of an older car (ISO 14230, fast init), reached with LINK_KLINE:
//   address 0x11 Engine (KWP)   : P0123 stored          address 0x64 Cluster (KWP) : no faults
// and one unit behind the gateway on VW TP 2.0 (frame-level simulation, see Tp20Ecu):
//   address 0x42 Door front left (KWP): one fault stored, long identification (multi-frame)
// The SecurityAccess keys use made-up test constants (demoKey below) - they unlock nothing but this simulator.
#pragma once
#include "bus.h"
#include "seedkey.h"
#include "tp20.h"
#include <cstring>
#include <deque>
#include <map>

namespace sim {

inline uint32_t klineKey(uint32_t addr) { return 0x40000000u | (addr & 0xFF); }      // K-line units in the ecus map
inline uint32_t tp20Key(uint32_t addr) { return 0x20000000u | (addr & 0xFF); }       // TP 2.0 units in the ecus map

static const char *const DEMO_VIN = "WP0ZZZ97ZCL012345";       // not a real car

// Test-only key function of the simulator: key = ((seed ^ K) * A + B) mod 2^(8*nb)
static const uint32_t DEMO_K4 = 0x5AA5C33Cu, DEMO_A4 = 0x00010DCDu, DEMO_B4 = 0x0BADC0DEu;   // 4-byte units (PASM)
static const uint32_t DEMO_K2 = 0x00003A5Cu, DEMO_A2 = 0x00000065u, DEMO_B2 = 0x00001D2Bu;   // 2-byte units (PDK)
inline uint32_t demoKey(uint32_t seed, int nb) {
    uint64_t mask = nb >= 4 ? 0xFFFFFFFFull : ((1ull << (8 * nb)) - 1);
    return nb >= 4 ? (uint32_t)((((seed ^ DEMO_K4) & mask) * DEMO_A4 + DEMO_B4) & mask)
                   : (uint32_t)((((seed ^ DEMO_K2) & mask) * DEMO_A2 + DEMO_B2) & mask);
}

// Replaces the loaded key file by the simulator's test keys, under the algorithm names the Panamera pack
// uses for PASM and PDK. Only for --demo and the tests.
inline void installDemoKeys() {
    sk::keys().clear();
    sk::Key k4; k4.fam = sk::FAM_KMUL; k4.nb = 4; k4.K = DEMO_K4; k4.A = DEMO_A4; k4.B = DEMO_B4; sk::keys()["ASAM_seedkey_PASM"] = k4;
    sk::Key k2; k2.fam = sk::FAM_KMUL; k2.nb = 2; k2.K = DEMO_K2; k2.A = DEMO_A2; k2.B = DEMO_B2; sk::keys()["ASAM_seedkey_PDK"] = k2;
    sk::loaded() = true;
}

struct Ecu {
    int proto = 1;                                 // 1 UDS, 2 KWP
    std::vector<std::vector<uint8_t>> faults;
    uint8_t session = 1; bool cleared = false; int pending = 0;
    // security access
    int saLevel = 0, saBytes = 0; bool unlocked = false; uint32_t seed = 0; int badKeys = 0; bool seedOut = false;
    uint32_t lcg = 0x1234567u;
    int routinePolls = 0; uint16_t height = 500; int resets = 0;
    bool teachFail = false; int teachPolls = -1; int teachIns = 0;     // PDK teach-in (KWP 31/33/32 20..22)
};

class Car {
public:
    std::map<uint32_t, Ecu> ecus;                  // keyed by request id
    std::map<uint32_t, uint32_t> resp;
    std::deque<std::pair<uint32_t, std::vector<uint8_t>>> rx;

    Car() {
        add(0x710, 0x77A, 1, {});
        add(0x714, 0x77E, 1, {});
        add(0x70F, 0x779, 2, {{0x18, 0x72, 0x20}});
        add(0x755, 0x7BF, 1, {{0x00, 0x01, 0x10, 0x09}});
        add(0x715, 0x77F, 1, {});
        add(0x753, 0x7BD, 2, {{0xC1, 0x55, 0x20}});
        add(0x7E0, 0x7E8, 2, {}); ecus[0x7E0].pending = 1;
        add(klineKey(0x11), 0xF1, 2, {{0x01, 0x23, 0x20}});
        add(klineKey(0x64), 0xF1, 2, {});
        add(tp20Key(0x42), 0x300, 2, {{0x93, 0x10, 0x20}});
        ecus[0x755].saLevel = 3; ecus[0x755].saBytes = 4;
        ecus[0x70F].saLevel = 1; ecus[0x70F].saBytes = 2;
    }
    void add(uint32_t tx, uint32_t rxid, int proto, std::vector<std::vector<uint8_t>> faults) {
        Ecu e; e.proto = proto; e.faults = std::move(faults); ecus[tx] = e; resp[tx] = rxid;
    }
    void reply(uint32_t id, std::vector<uint8_t> p) { rx.push_back({id, std::move(p)}); }
    bool hasKline(uint32_t addr) const { return ecus.count(klineKey(addr)) != 0; }

    void handle(uint32_t tx, const uint8_t *q, size_t n) {
        auto it = ecus.find(tx); if (it == ecus.end() || n == 0) return;        // module absent: silence
        Ecu &e = it->second; uint32_t rid = resp[tx];
        uint8_t sid = q[0]; uint8_t q1 = n > 1 ? q[1] : 0, q2 = n > 2 ? q[2] : 0, q3 = n > 3 ? q[3] : 0;
        auto nrc = [&](uint8_t c) { reply(rid, {0x7F, sid, c}); };
        if (e.pending > 0 && sid == 0x18) { e.pending--; nrc(0x78); }
        if (sid == 0x10) {
            if (q1 != e.session) { e.unlocked = false; e.seedOut = false; }      // a session change locks the unit again
            e.session = q1; reply(rid, {0x50, q1}); return;
        }
        if (sid == 0x27 && e.saLevel) {                                         // SecurityAccess
            if (e.proto == 1 && e.session != 3) return nrc(0x7F);
            if (q1 == e.saLevel) {                                              // request seed
                if (e.badKeys >= 2) return nrc(0x37);
                std::vector<uint8_t> r = {0x67, q1};
                if (e.unlocked) e.seed = 0;
                else { e.lcg = e.lcg * 1103515245u + 12345u; e.seed = (e.lcg >> 8) | 1u; if (e.saBytes == 2) e.seed &= 0xFFFF; }
                for (int i = e.saBytes - 1; i >= 0; i--) r.push_back((uint8_t)(e.seed >> (8 * i)));
                e.seedOut = true; reply(rid, r); return;
            }
            if (q1 == e.saLevel + 1) {                                          // send key
                if (!e.seedOut) return nrc(0x24);
                if ((int)n != 2 + e.saBytes) return nrc(0x13);
                uint32_t key = 0; for (int i = 0; i < e.saBytes; i++) key = (key << 8) | q[2 + i];
                e.seedOut = false;
                if (key != demoKey(e.seed, e.saBytes)) { e.badKeys++; return nrc(e.badKeys >= 2 ? 0x36 : 0x35); }
                e.unlocked = true; e.badKeys = 0;
                if (e.proto == 2) reply(rid, {0x67, q1, 0x34}); else reply(rid, {0x67, q1});
                return;
            }
            return nrc(0x12);
        }
        if (e.proto == 1) {                                                     // ---- UDS
            if (sid == 0x19 && q1 == 0x02) { std::vector<uint8_t> r = {0x59, 0x02, 0xFF};
                if (!e.cleared) { for (auto &f : e.faults) r.insert(r.end(), f.begin(), f.end()); }
                reply(rid, r); return; }
            if (sid == 0x19 && q1 == 0x06 && n >= 6) {                          // extended data (environment) of one fault
                for (auto &f : e.faults) if (!e.cleared && f[0] == q[2] && f[1] == q[3] && f[2] == q[4]) {
                    // record 01: frequency 3, unlearn counter 40 | record 02: 123456 km, 2012-06-15 14:30:05, 12.4 V
                    reply(rid, {0x59, 0x06, q[2], q[3], q[4], f[3], 0x01, 0x03, 0x28, 0x02, 0x01, 0xE2, 0x40, 0x00,
                                0x00, 0x31, 0x9E, 0xE7, 0x85, 0x3E});
                    return;
                }
                return nrc(0x31);
            }
            if (sid == 0x19 && q1 == 0x04 && n >= 6) {                          // snapshot: ride height when the fault was stored
                for (auto &f : e.faults) if (!e.cleared && f[0] == q[2] && f[1] == q[3] && f[2] == q[4] && tx == 0x755) {
                    reply(rid, {0x59, 0x04, q[2], q[3], q[4], f[3], 0x01, 0x01, 0x00, 0xA0, 0x01, 0xC2});   // 00A0 = 450 mm
                    return;
                }
                return nrc(0x31);
            }
            if (sid == 0x22 && q1 == 0xF1 && q2 == 0x90 && (tx == 0x710 || tx == 0x714)) {
                std::vector<uint8_t> r = {0x62, 0xF1, 0x90}; r.insert(r.end(), DEMO_VIN, DEMO_VIN + 17); reply(rid, r); return; }
            if (sid == 0x22 && tx == 0x755 && q1 == 0x00 && q2 == 0xA0) { reply(rid, {0x62, 0x00, 0xA0, (uint8_t)(e.height >> 8), (uint8_t)e.height}); return; }
            if (sid == 0x22 && tx == 0x755 && q1 == 0x04 && q2 == 0xA0) { reply(rid, {0x62, 0x04, 0xA0, 0x07}); return; }
            if (sid == 0x2E && tx == 0x755 && q1 == 0x00 && q2 == 0xA0) {       // ride height write: session + security
                if (e.session != 3) return nrc(0x7F);
                if (!e.unlocked) return nrc(0x33);
                if (n < 5) return nrc(0x13);
                e.height = (uint16_t)((q[3] << 8) | q[4]); reply(rid, {0x6E, 0x00, 0xA0}); return;
            }
            if (sid == 0x22) { const char *v = tx == 0x755 ? "97034301234" : tx == 0x714 ? "97064111500" : tx == 0x710 ? "97061812001" : "4H0959655";
                std::vector<uint8_t> r = {0x62, q1, q2}; r.insert(r.end(), v, v + strlen(v)); reply(rid, r); return; }
            if (sid == 0x31 && tx == 0x755 && q2 == 0xF1 && q3 == 0x05) {       // level the car
                if (e.session != 3) return nrc(0x7F);
                if (q1 == 1) { e.routinePolls = 0; reply(rid, {0x71, 0x01, 0xF1, 0x05}); return; }
                if (q1 == 3) { e.routinePolls++; reply(rid, {0x71, 0x03, 0xF1, 0x05, (uint8_t)(e.routinePolls < 3 ? 1 : 2)}); return; }
                if (q1 == 2) { reply(rid, {0x71, 0x02, 0xF1, 0x05}); return; }
            }
            if (sid == 0x31 && tx == 0x714 && q2 == 0x04 && q3 == 0x50) {       // service interval reset
                if (e.session != 3) return nrc(0x7F);
                if (q1 == 1) { e.resets++; reply(rid, {0x71, 0x01, 0x04, 0x50}); return; }
                if (q1 == 3) { reply(rid, {0x71, 0x03, 0x04, 0x50, 0x02}); return; }
            }
            if (sid == 0x3E) { if (!(q1 & 0x80)) reply(rid, {0x7E, 0x00}); return; }
            if (sid == 0x14) { e.cleared = true; reply(rid, {0x54}); return; }
            return nrc(0x11);
        }
        // ---- KWP2000
        if (sid == 0x19) return nrc(0x11);
        if (sid == 0x18) { std::vector<uint8_t> r = {0x58, (uint8_t)(e.cleared ? 0 : e.faults.size())};
            if (!e.cleared) { for (auto &f : e.faults) r.insert(r.end(), f.begin(), f.end()); }
            reply(rid, r); return; }
        if (sid == 0x1A && q1 == 0x90 && tx == 0x7E0) { std::vector<uint8_t> r = {0x5A, 0x90}; r.insert(r.end(), DEMO_VIN, DEMO_VIN + 17); reply(rid, r); return; }
        if (sid == 0x1A && tx == tp20Key(0x42)) { const char *v = "7PP959701  Tuersteuergeraet FS H07 0130"; std::vector<uint8_t> r = {0x5A, q1};
            r.insert(r.end(), v, v + strlen(v)); reply(rid, r); return; }
        if (sid == 0x1A) { const char *v = "970 PDK 3000  G088"; std::vector<uint8_t> r = {0x5A, q1};
            r.insert(r.end(), v, v + strlen(v)); reply(rid, r); return; }
        if (tx == 0x70F && (sid == 0x31 || sid == 0x33 || sid == 0x32) && (q1 == 0x10 || (q1 >= 0x20 && q1 <= 0x22))) {   // oil fill mode, teach-ins
            if (sid == 0x31) { e.teachPolls = 0; e.teachIns++; reply(rid, {0x71, q1}); return; }
            if (sid == 0x32) { e.teachPolls = -1; reply(rid, {0x72, q1, 0x02}); return; }
            if (e.teachPolls < 0) { reply(rid, {0x73, q1, 0x00}); return; }               // not started
            int k = ++e.teachPolls;
            // result: 1 active, then 2 finished - or 4 "fault stored" with "selector in P" and "system pressure" not met
            uint8_t res = k < 3 ? 1 : (e.teachFail ? 4 : 2);
            uint8_t c6 = e.teachFail ? 0x6C : 0x7F;                                        // byte 6: bits 1 and 4 cleared on failure
            reply(rid, {0x73, q1, res, 0x00, 0x0F, 0xFF, c6, 0x00, 0x0F, 0xFF, 0xFF, (uint8_t)(k < 3 ? 0x40 : 0xFF),
                        0x0A, 0x00, 0x0B, 0x00, 0x0C, 0x00, 0x0D, 0x00});
            return;
        }
        if (sid == 0x3E) { reply(rid, {0x7E}); return; }
        if (sid == 0x14) { if (tx == 0x70F && e.session != 0x89) return nrc(0x22); e.cleared = true; reply(rid, {0x54, 0xFF, 0x00}); return; }
        nrc(0x11);
    }
};

// ECU side of VW TP 2.0 at frame level: channel setup, parameters, numbered data frames with ACKs,
// keep-alive and disconnect. Payloads go to Car::handle under tp20Key(address).
class Tp20Ecu {
    Car &car; uint8_t addr = 0; bool open = false; uint32_t ecuRx = 0, testerRx = 0x300;
    uint8_t rxSeq = 0, txSeq = 0; std::vector<uint8_t> part; size_t partLen = 0; bool started = false;
public:
    std::deque<pt::CanFrame> out;
    explicit Tp20Ecu(Car &c) : car(c) {}
    void frameOut(uint32_t id, std::vector<uint8_t> d) { pt::CanFrame f; f.id = id; f.d = std::move(d); out.push_back(f); }
    void sendMessage(const std::vector<uint8_t> &m) {
        std::vector<uint8_t> body = {(uint8_t)(m.size() >> 8), (uint8_t)m.size()}; body.insert(body.end(), m.begin(), m.end());
        for (size_t p = 0; p < body.size();) {
            size_t n = std::min<size_t>(7, body.size() - p); bool last = p + n >= body.size();
            std::vector<uint8_t> f = {(uint8_t)((last ? 0x10 : 0x20) | txSeq)}; f.insert(f.end(), body.begin() + p, body.begin() + p + n);
            frameOut(testerRx, f); txSeq = (uint8_t)((txSeq + 1) & 0x0F); p += n;
        }
    }
    void handle(const pt::CanFrame &f) {
        if (f.id == 0x200 && f.d.size() >= 7 && f.d[1] == 0xC0) {                       // channel setup
            uint8_t a = f.d[0];
            if (!car.ecus.count(tp20Key(a))) return;                                    // nobody at this address
            addr = a; open = true; ecuRx = 0x740 + (a & 0x3F); rxSeq = txSeq = 0; part.clear(); partLen = 0; started = false;
            testerRx = (uint32_t)(f.d[4] | ((f.d[5] & 0x07) << 8));
            frameOut(0x200u + a, {0x00, 0xD0, (uint8_t)(ecuRx & 0xFF), (uint8_t)(ecuRx >> 8), (uint8_t)(testerRx & 0xFF), (uint8_t)(testerRx >> 8), 0x01});
            return;
        }
        if (!open || f.id != ecuRx || f.d.empty()) return;
        uint8_t op = f.d[0] >> 4;
        if (f.d[0] == 0xA0) { frameOut(testerRx, {0xA1, 0x0F, 0x8A, 0xFF, 0x4A, 0xFF}); return; }
        if (f.d[0] == 0xA3) { frameOut(testerRx, {0xA1, 0x0F, 0x8A, 0xFF, 0x4A, 0xFF}); return; }
        if (f.d[0] == 0xA8) { frameOut(testerRx, {0xA8}); open = false; return; }
        if (op == 0xB || op == 0x9) return;                                              // tester's ACK
        if (op > 3) return;
        rxSeq = (uint8_t)(((f.d[0] & 0x0F) + 1) & 0x0F);
        size_t i = 1;
        if (!started) { if (f.d.size() < 3) return; partLen = (size_t)((f.d[1] << 8) | f.d[2]); i = 3; started = true; }
        part.insert(part.end(), f.d.begin() + i, f.d.end());
        if (op == 0 || op == 1) frameOut(testerRx, {(uint8_t)(0xB0 | rxSeq)});
        if (op == 1 || op == 3) {
            if (part.size() > partLen) part.resize(partLen);
            std::vector<uint8_t> msg = part; part.clear(); partLen = 0; started = false;
            car.rx.clear(); car.handle(tp20Key(addr), msg.data(), msg.size());
            while (!car.rx.empty()) { sendMessage(car.rx.front().second); car.rx.pop_front(); }
        }
    }
};

// ECU side of ISO 15765-2 at frame level (for adapters that only move CAN frames, e.g. the SLCAN test):
// requests on a unit's request id are reassembled (with flow control), answers sent as single or
// first + consecutive frames after the tester's flow control.
class IsoTpEcu {
    Car &car;
    struct Rx { std::vector<uint8_t> buf; size_t len = 0; uint8_t sn = 1; };
    std::map<uint32_t, Rx> rx;
    struct Tx { uint32_t id = 0; std::vector<uint8_t> msg; size_t pos = 0; uint8_t sn = 1; };
    std::deque<Tx> waitingFc;
public:
    std::deque<pt::CanFrame> out;
    explicit IsoTpEcu(Car &c) : car(c) {}
    void frameOut(uint32_t id, std::vector<uint8_t> d) { d.resize(8, 0x55); pt::CanFrame f; f.id = id; f.d = std::move(d); out.push_back(f); }
    void respond(uint32_t req, const std::vector<uint8_t> &m) {
        uint32_t id = car.resp[req];
        if (m.size() <= 7) { std::vector<uint8_t> d = {(uint8_t)m.size()}; d.insert(d.end(), m.begin(), m.end()); frameOut(id, d); return; }
        std::vector<uint8_t> d = {(uint8_t)(0x10 | (m.size() >> 8)), (uint8_t)m.size()}; d.insert(d.end(), m.begin(), m.begin() + 6);
        frameOut(id, d); Tx t; t.id = id; t.msg = m; t.pos = 6; waitingFc.push_back(t);
    }
    void handle(const pt::CanFrame &f) {
        uint32_t id = f.id & 0x1FFFFFFF;
        if (f.d.empty()) return;
        uint8_t t = f.d[0] >> 4;
        if (t == 3) {                                                                   // tester flow control
            for (auto it = waitingFc.begin(); it != waitingFc.end(); ++it) {
                Tx tx = *it; waitingFc.erase(it);
                while (tx.pos < tx.msg.size()) {
                    size_t n = std::min<size_t>(7, tx.msg.size() - tx.pos);
                    std::vector<uint8_t> d = {(uint8_t)(0x20 | tx.sn)}; d.insert(d.end(), tx.msg.begin() + tx.pos, tx.msg.begin() + tx.pos + n);
                    frameOut(tx.id, d); tx.pos += n; tx.sn = (uint8_t)((tx.sn + 1) & 0x0F);
                }
                break;
            }
            return;
        }
        if (!car.ecus.count(id)) return;                                                // nobody at this id
        std::vector<uint8_t> msg;
        if (t == 0) { size_t n = f.d[0] & 0x0F; if (!n || n + 1 > f.d.size()) return; msg.assign(f.d.begin() + 1, f.d.begin() + 1 + n); }
        else if (t == 1) { Rx &r = rx[id]; r.len = (size_t)((f.d[0] & 0x0F) << 8 | f.d[1]); r.buf.assign(f.d.begin() + 2, f.d.end()); r.sn = 1;
            frameOut(car.resp[id], {0x30, 0x00, 0x00}); return; }
        else if (t == 2) { Rx &r = rx[id]; r.buf.insert(r.buf.end(), f.d.begin() + 1, f.d.end());
            if (r.buf.size() < r.len) return;
            r.buf.resize(r.len); msg = r.buf; rx.erase(id); }
        else return;
        car.rx.clear(); car.handle(id, msg.data(), msg.size());
        while (!car.rx.empty()) { respond(id, car.rx.front().second); car.rx.pop_front(); }
    }
};

// The simulated car as a transport.
class SimBus : public pt::Bus, public pt::RawCan {
public:
    Car car; Tp20Ecu tpEcu{car}; pt::Tp20 tp{*this};
    SimBus() { fast = true; }
    bool open() override { return true; }
    void close() override { tp.close(); }
    // raw CAN side, used by the TP 2.0 layer
    bool rawOpen() override { return true; }
    void rawClose() override {}
    bool sendFrame(const pt::CanFrame &f) override { tpEcu.handle(f); return true; }
    bool recvFrame(pt::CanFrame &f, unsigned) override { if (tpEcu.out.empty()) return false; f = tpEcu.out.front(); tpEcu.out.pop_front(); return true; }
    bool target(uint32_t tx, uint32_t rx, int k = pt::LINK_CAN) override {
        car.rx.clear(); pt::Bus::target(tx, rx, k);
        if (k == pt::LINK_TP20) tp.open((uint8_t)tx); else tp.close();
        return true;
    }
    bool sendRaw(const std::vector<uint8_t> &p) override {
        if (kind == pt::LINK_TP20) return tp.sendMessage(p);
        car.handle(kind == pt::LINK_KLINE ? klineKey(txId) : txId, p.data(), p.size()); return true; }
    bool recvRaw(std::vector<uint8_t> &out, unsigned tmo) override {
        if (kind == pt::LINK_TP20) return tp.recvMessage(out, tmo > 50 ? 50 : tmo);
        while (!car.rx.empty()) {
            auto m = car.rx.front(); car.rx.pop_front();
            if (m.first == rxId) { out = m.second; return true; }
        }
        return false;
    }
};

} // namespace sim
