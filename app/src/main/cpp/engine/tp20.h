// VW TP 2.0 transport (the "sub-bus" control units behind the gateway: doors, seats, auxiliary heater,
// trailer hitch, rear camera, lane-change assistant on Panamera 970, Cayenne 92A, Macan, 991 ...).
// Runs on top of plain CAN frames, so every adapter that can send raw CAN frames can use it.
//
//   channel setup   tester -> 0x200       : <ECU addr> C0 00 10 00 03 01   (tester receives on 0x300, app type KWP)
//                   ECU    -> 0x200+addr  : 00 D0 <tx lo> <tx hi> <rx lo> <rx hi> 01
//   parameters      tester -> tx          : A0 0F 8A FF 32 FF                (block size 15, T1 100 ms, T3 5 ms)
//                   ECU    -> 0x300       : A1 0F 8A FF 4A FF
//   data frames     <op|seq> <data...>    op 0x0 wait for ACK, more follow · 0x1 wait for ACK, last
//                                         op 0x2 no ACK, more follow     · 0x3 no ACK, last
//                   the first frame of a message carries the length (2 bytes, big endian)
//   acknowledge     0xB0 | next seq       (0x90 | next seq = not ready)
//   keep alive      A3 -> A1 ...          disconnect A8 -> A8
#pragma once
#include "bus.h"
#include <deque>

namespace pt {

struct CanFrame { uint32_t id = 0; std::vector<uint8_t> d; };

// Plain CAN frame access, provided by the adapter (J2534 "CAN" protocol, SocketCAN raw socket, simulator).
class RawCan {
public:
    virtual ~RawCan() {}
    virtual bool rawOpen() = 0;                 // switch the adapter to raw CAN frames
    virtual void rawClose() = 0;
    virtual bool sendFrame(const CanFrame &f) = 0;
    virtual bool recvFrame(CanFrame &f, unsigned timeoutMs) = 0;
};

class Tp20 {
    RawCan &can;
    uint32_t txId = 0, rxId = 0x300; uint8_t addr = 0;
    uint8_t txSeq = 0, rxSeq = 0; bool up = false; uint32_t lastTx = 0;
    std::deque<std::vector<uint8_t>> pending;    // complete messages received
    std::vector<uint8_t> part; size_t partLen = 0;
    bool frame(CanFrame &f, unsigned tmo) {
        uint32_t t0 = nowMs();
        while (nowMs() - t0 < tmo) {
            if (!can.recvFrame(f, tmo - (nowMs() - t0))) return false;
            if (f.id == rxId && !f.d.empty()) return true;
        }
        return false;
    }
    bool send(const std::vector<uint8_t> &d) { CanFrame f; f.id = txId; f.d = d; lastTx = nowMs(); return can.sendFrame(f); }
    // handle one received data frame; returns true when a message completed
    bool take(const CanFrame &f) {
        uint8_t op = f.d[0] >> 4, seq = f.d[0] & 0x0F;
        if (op == 0xA) {                                         // channel parameters / keep alive answer / disconnect
            if (f.d[0] == 0xA8) up = false;
            return false;
        }
        if (op == 0xB || op == 0x9) return false;                // ACK for us (handled in sendMessage)
        if (op > 0x3) return false;
        rxSeq = (uint8_t)((seq + 1) & 0x0F);
        size_t i = 1;
        if (part.empty() && partLen == 0) {
            if (f.d.size() < 3) return false;
            partLen = (size_t)((f.d[1] << 8) | f.d[2]) & 0x7FFF; i = 3;
        }
        part.insert(part.end(), f.d.begin() + i, f.d.end());
        if (op == 0x0 || op == 0x1) send({(uint8_t)(0xB0 | rxSeq)});   // acknowledge
        if (op == 0x1 || op == 0x3) {
            if (part.size() > partLen) part.resize(partLen);
            pending.push_back(part); part.clear(); partLen = 0;
            return true;
        }
        return false;
    }
public:
    explicit Tp20(RawCan &c) : can(c) {}
    bool connected() const { return up; }
    uint8_t address() const { return addr; }
    // Open a channel to the control unit at VW diagnostic address 'a'. False = nobody answered.
    bool open(uint8_t a) {
        close();
        addr = a; txSeq = rxSeq = 0; pending.clear(); part.clear(); partLen = 0;
        CanFrame s; s.id = 0x200; s.d = {a, 0xC0, 0x00, 0x10, 0x00, 0x03, 0x01};
        if (!can.sendFrame(s)) return false;
        CanFrame r; uint32_t t0 = nowMs();
        while (nowMs() - t0 < 300) {
            if (!can.recvFrame(r, 300 - (nowMs() - t0))) return false;
            if (r.id == 0x200u + a && r.d.size() >= 7 && r.d[1] == 0xD0) {
                txId = (uint32_t)(r.d[2] | ((r.d[3] & 0x07) << 8));
                rxId = (uint32_t)(r.d[4] | ((r.d[5] & 0x07) << 8));
                if (rxId == 0) rxId = 0x300;
                break;
            }
        }
        if (!txId) return false;
        if (!send({0xA0, 0x0F, 0x8A, 0xFF, 0x32, 0xFF})) return false;
        CanFrame p;
        if (!frame(p, 300) || p.d[0] != 0xA1) return false;
        up = true; return true;
    }
    void close() {
        if (up) { send({0xA8}); CanFrame f; frame(f, 50); }
        up = false; txId = 0;
    }
    void keepAlive() {
        if (!up || nowMs() - lastTx < 1000) return;
        send({0xA3}); CanFrame f;
        if (frame(f, 200) && f.d[0] != 0xA1) take(f);
    }
    // Send one message (split into frames, waiting for the ACK after each block of 15).
    bool sendMessage(const std::vector<uint8_t> &m) {
        if (!up) return false;
        std::vector<uint8_t> body = {(uint8_t)(m.size() >> 8), (uint8_t)m.size()}; body.insert(body.end(), m.begin(), m.end());
        size_t pos = 0; int inBlock = 0;
        while (pos < body.size()) {
            size_t n = std::min<size_t>(7, body.size() - pos); bool last = pos + n >= body.size();
            bool ack = last || ++inBlock >= 15;
            uint8_t op = last ? (ack ? 0x10 : 0x30) : (ack ? 0x00 : 0x20);
            std::vector<uint8_t> f = {(uint8_t)(op | txSeq)}; f.insert(f.end(), body.begin() + pos, body.begin() + pos + n);
            if (!send(f)) return false;
            txSeq = (uint8_t)((txSeq + 1) & 0x0F); pos += n;
            if (ack) {
                inBlock = 0; CanFrame a; uint32_t t0 = nowMs(); bool ok = false;
                while (nowMs() - t0 < 500 && frame(a, 500)) {
                    uint8_t o = a.d[0] >> 4;
                    if (o == 0xB) { ok = true; break; }
                    if (o == 0x9) { t0 = nowMs(); continue; }                 // not ready: wait
                    take(a);                                                     // ECU already answering
                }
                if (!ok) return false;
            }
        }
        return true;
    }
    bool recvMessage(std::vector<uint8_t> &out, unsigned tmo) {
        uint32_t t0 = nowMs();
        while (pending.empty() && nowMs() - t0 < tmo) {
            CanFrame f; if (!frame(f, tmo - (nowMs() - t0))) break;
            take(f);
        }
        if (pending.empty()) return false;
        out = pending.front(); pending.pop_front(); return true;
    }
};

} // namespace pt
