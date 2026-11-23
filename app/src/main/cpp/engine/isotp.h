// ISO 15765-2 (ISO-TP) in user space, on top of plain CAN frames: for adapters that only move frames
// (SLCAN serial adapters on macOS / Linux). Frames are always padded to 8 bytes, like the control units expect.
//   single frame      0L data...                 (L <= 7)
//   first frame       1L LL data...               (12-bit length)
//   consecutive       2N data...                  (N = sequence 1..F,0..)
//   flow control      30 BS STmin                 (we send BS 0 = no limit, STmin 0)
#pragma once
#include "tp20.h"

namespace pt {

class IsoTp {
    RawCan &can;
public:
    uint32_t txId = 0, rxId = 0; bool ext = false;
    explicit IsoTp(RawCan &c) : can(c) {}
    void setIds(uint32_t tx, uint32_t rx) { txId = tx; rxId = rx; ext = tx > 0x7FF || rx > 0x7FF; }
    bool frame(const std::vector<uint8_t> &d) {
        CanFrame f; f.id = txId | (ext ? 0x80000000u : 0); f.d = d; f.d.resize(8, 0xAA);
        return can.sendFrame(f);
    }
    bool wait(CanFrame &f, unsigned tmo) {
        uint32_t t0 = nowMs();
        while (nowMs() - t0 < tmo) {
            if (!can.recvFrame(f, tmo - (nowMs() - t0))) return false;
            if ((f.id & 0x1FFFFFFF) == rxId && !f.d.empty()) return true;
        }
        return false;
    }
    bool send(const std::vector<uint8_t> &p) {
        if (p.size() <= 7) { std::vector<uint8_t> d = {(uint8_t)p.size()}; d.insert(d.end(), p.begin(), p.end()); return frame(d); }
        if (p.size() > 4095) return false;
        std::vector<uint8_t> d = {(uint8_t)(0x10 | (p.size() >> 8)), (uint8_t)p.size()}; d.insert(d.end(), p.begin(), p.begin() + 6);
        if (!frame(d)) return false;
        size_t pos = 6; uint8_t sn = 1; int bs = 0, sent = 0; unsigned stmin = 0;
        auto flow = [&]() -> bool {
            CanFrame fc;
            for (;;) {
                if (!wait(fc, 1000) || (fc.d[0] >> 4) != 3) return false;
                if ((fc.d[0] & 0x0F) == 1) continue;                    // wait
                if ((fc.d[0] & 0x0F) == 2) return false;                // overflow
                bs = fc.d.size() > 1 ? fc.d[1] : 0; stmin = fc.d.size() > 2 && fc.d[2] <= 0x7F ? fc.d[2] : 1; sent = 0;
                return true;
            }
        };
        if (!flow()) return false;
        while (pos < p.size()) {
            size_t n = std::min<size_t>(7, p.size() - pos);
            std::vector<uint8_t> c = {(uint8_t)(0x20 | sn)}; c.insert(c.end(), p.begin() + pos, p.begin() + pos + n);
            if (!frame(c)) return false;
            pos += n; sn = (uint8_t)((sn + 1) & 0x0F);
            if (stmin) sleepMs(stmin);
            if (bs && ++sent >= bs && pos < p.size() && !flow()) return false;
        }
        return true;
    }
    bool recv(std::vector<uint8_t> &out, unsigned tmo) {
        CanFrame f;
        if (!wait(f, tmo)) return false;
        uint8_t t = f.d[0] >> 4;
        if (t == 0) { size_t n = f.d[0] & 0x0F; if (n == 0 || n + 1 > f.d.size()) return false; out.assign(f.d.begin() + 1, f.d.begin() + 1 + n); return true; }
        if (t != 1 || f.d.size() < 8) return false;
        size_t len = (size_t)((f.d[0] & 0x0F) << 8 | f.d[1]);
        out.assign(f.d.begin() + 2, f.d.end());
        frame({0x30, 0x00, 0x00});                                      // flow control: send everything
        uint8_t sn = 1;
        while (out.size() < len) {
            if (!wait(f, 1000) || (f.d[0] >> 4) != 2 || (f.d[0] & 0x0F) != sn) return false;
            out.insert(out.end(), f.d.begin() + 1, f.d.end());
            sn = (uint8_t)((sn + 1) & 0x0F);
        }
        out.resize(len);
        return true;
    }
};

} // namespace pt
