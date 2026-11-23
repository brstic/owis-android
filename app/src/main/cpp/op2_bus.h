// Tactrix OpenPort 2.0 transport for OWIS. The device protocol and J2534 PassThru implementation come from
// the vendored Apache-2.0 library github.com/roffe/libj2534_openport2 (app/src/main/cpp/op2lib/op2.c); only
// its serial byte I/O is replaced by Android USB bulk transfers (the op2_usb_* hooks, defined in owis_jni.cpp,
// built with -DOP2_ANDROID_USB). This class drives that library's PassThru API and runs OWIS's own ISO-TP
// (ISO 15765-2) and VW TP 2.0 on top of the raw CAN frames, exactly as the SLCAN transport does.
#pragma once
#include "engine/bus.h"
#include "engine/isotp.h"
#include <algorithm>
#include <cstring>
#include <functional>

extern "C" {
#include "op2lib/j2534.h"
}

namespace pt {

// A raw byte pipe to the adapter (Android: UsbDeviceConnection bulk transfers via JNI).
struct UsbPipe {
    std::function<int(const uint8_t *, int)> write;    // returns bytes written (<0 error)
    std::function<int(uint8_t *, int, int)> read;      // buf, max, timeoutMs -> bytes (0 timeout, <0 error)
};

// op2.c drives a single global device, so its byte I/O is routed to whichever Op2Bus is open.
inline UsbPipe *&op2ActivePipe() { static UsbPipe *p = nullptr; return p; }

class Op2Bus : public Bus, public RawCan {
    UsbPipe usb;
    IsoTp iso{*this};
    Tp20 tp{*this};
    uint32_t dev = 0, chan = 0, filt = 0, kchan = 0, kfilt = 0;
    bool opened = false, kOpen = false, kUp = false;

    // J2534-1 04.04 constants (the library header names CAN/ISO14230/PASS_FILTER/FAST_INIT/CLEAR_RX_BUFFER).
    enum { J_CAN = CAN, J_ISO14230 = ISO14230, J_PASS_FILTER = PASS_FILTER, J_CAN_29BIT_ID = 0x100,
           J_TX_MSG_TYPE = 0x01, J_START_OF_MESSAGE = 0x02 };
    static void putId(PASSTHRU_MSG &m, uint32_t id) {
        m.Data[0] = (id >> 24) & 0xFF; m.Data[1] = (id >> 16) & 0xFF; m.Data[2] = (id >> 8) & 0xFF; m.Data[3] = id & 0xFF;
    }

    // K-line (ISO 14230 / KWP2000) for the older cars, mirroring the desktop J2534 path. The OpenPort 2.0 library
    // currently stubs the fast-init ioctl, so this is wired but unconfirmed — the same maturity as the desktop,
    // whose K-line is also pending on-car confirmation.
    bool kOpenChannel() {
        if (kOpen) return true;
        if (PassThruConnect(dev, J_ISO14230, 0, 10400, &kchan) != STATUS_NOERROR) {
            error("[ERROR] the adapter cannot open the K-line (ISO 14230)."); return false;
        }
        PASSTHRU_MSG mask, patt; memset(&mask, 0, sizeof mask); memset(&patt, 0, sizeof patt);
        mask.ProtocolID = patt.ProtocolID = J_ISO14230; mask.DataSize = patt.DataSize = 1;
        PassThruStartMsgFilter(kchan, J_PASS_FILTER, &mask, &patt, nullptr, &kfilt);
        kOpen = true; return true;
    }
    void kInit() {
        PassThruIoctl(kchan, CLEAR_RX_BUFFER, nullptr, nullptr);
        PASSTHRU_MSG in, out; memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
        in.ProtocolID = out.ProtocolID = J_ISO14230;
        in.Data[0] = 0x81; in.Data[1] = (uint8_t)txId; in.Data[2] = (uint8_t)rxId; in.Data[3] = 0x81; in.DataSize = 4;
        int32_t rc = PassThruIoctl(kchan, FAST_INIT, &in, &out);
        if (rc != STATUS_NOERROR) error("[K-line] this adapter/library does not do the fast-init itself; trying anyway.");
        kUp = true;   // best effort: attempt the exchange even if the library stubbed the init
    }

public:
    explicit Op2Bus(const UsbPipe &pipe) : usb(pipe) {}
    ~Op2Bus() override { close(); }

    bool open() override {
        op2ActivePipe() = &usb;
        if (PassThruOpen(nullptr, &dev) != STATUS_NOERROR) {
            error("[ERROR] the OpenPort 2.0 did not answer (cable and ignition on?)."); op2ActivePipe() = nullptr; return false;
        }
        if (PassThruConnect(dev, J_CAN, 0, 500000, &chan) != STATUS_NOERROR) {
            error("[ERROR] the adapter cannot open the CAN channel."); PassThruClose(dev); op2ActivePipe() = nullptr; return false;
        }
        PASSTHRU_MSG mask, patt; memset(&mask, 0, sizeof mask); memset(&patt, 0, sizeof patt);
        mask.ProtocolID = patt.ProtocolID = J_CAN; mask.DataSize = patt.DataSize = 4;     // pass-all: mask 0, pattern 0
        PassThruStartMsgFilter(chan, J_PASS_FILTER, &mask, &patt, nullptr, &filt);
        opened = true;
        return true;
    }
    void close() override {
        if (tp.connected()) tp.close();
        if (kOpen) { PassThruDisconnect(kchan); kOpen = kUp = false; }
        if (opened) { PassThruDisconnect(chan); PassThruClose(dev); opened = false; }
        op2ActivePipe() = nullptr;
    }

    // ---- RawCan ----
    bool rawOpen() override { return opened; }
    void rawClose() override {}
    bool sendFrame(const CanFrame &f) override {
        bool ext = (f.id & 0x80000000u) || (f.id & 0x1FFFFFFF) > 0x7FF;
        uint32_t id = f.id & 0x1FFFFFFF;
        PASSTHRU_MSG m; memset(&m, 0, sizeof m);
        m.ProtocolID = J_CAN; m.TxFlags = ext ? J_CAN_29BIT_ID : 0;
        putId(m, id);
        size_t n = std::min<size_t>(8, f.d.size());
        if (n) memcpy(m.Data + 4, f.d.data(), n);
        m.DataSize = (uint32_t)(4 + n);
        uint32_t num = 1;
        return PassThruWriteMsgs(chan, &m, &num, 100) == STATUS_NOERROR;
    }
    bool recvFrame(CanFrame &f, unsigned timeoutMs) override {
        uint32_t t0 = nowMs();
        for (;;) {
            PASSTHRU_MSG m; memset(&m, 0, sizeof m); uint32_t num = 1;
            int left = (int)timeoutMs - (int)(nowMs() - t0); if (left < 0) left = 0;
            if (PassThruReadMsgs(chan, &m, &num, (uint32_t)left) == STATUS_NOERROR && num > 0 && m.DataSize >= 4) {
                if (!(m.RxStatus & J_TX_MSG_TYPE)) {                 // skip the loopback echo of our own TX
                    f.id = ((uint32_t)m.Data[0] << 24) | ((uint32_t)m.Data[1] << 16) | ((uint32_t)m.Data[2] << 8) | m.Data[3];
                    f.d.assign(m.Data + 4, m.Data + m.DataSize);
                    return true;
                }
            }
            if (nowMs() - t0 >= timeoutMs) return false;
        }
    }

    // ---- Bus (ISO-TP / VW TP 2.0 on top of the frames) ----
    bool target(uint32_t tx, uint32_t rx, int linkKind = LINK_CAN) override {
        if (tp.connected() && (linkKind != LINK_TP20 || tx != tp.address())) tp.close();
        Bus::target(tx, rx, linkKind);
        if (linkKind == LINK_KLINE) { if (!kOpenChannel()) return false; kInit(); return true; }
        if (linkKind == LINK_TP20) { if (!tp.connected()) tp.open((uint8_t)tx); }
        else iso.setIds(tx, rx);
        return opened;
    }
    bool sendRaw(const std::vector<uint8_t> &p) override {
        if (kind == LINK_TP20) { tp.keepAlive(); return tp.sendMessage(p); }
        if (kind == LINK_KLINE) {
            if (!kUp || p.size() > 255) return false;
            PASSTHRU_MSG m; memset(&m, 0, sizeof m); m.ProtocolID = J_ISO14230; size_t h = 0;
            if (p.size() < 64) { m.Data[h++] = (uint8_t)(0x80 | p.size()); m.Data[h++] = (uint8_t)txId; m.Data[h++] = (uint8_t)rxId; }
            else { m.Data[h++] = 0x80; m.Data[h++] = (uint8_t)txId; m.Data[h++] = (uint8_t)rxId; m.Data[h++] = (uint8_t)p.size(); }
            memcpy(m.Data + h, p.data(), p.size()); m.DataSize = (uint32_t)(h + p.size());
            uint32_t num = 1; return PassThruWriteMsgs(kchan, &m, &num, 1000) == STATUS_NOERROR;   // adapter appends the checksum
        }
        return iso.send(p);
    }
    bool recvRaw(std::vector<uint8_t> &out, unsigned timeoutMs) override {
        if (kind == LINK_TP20) return tp.recvMessage(out, timeoutMs);
        if (kind == LINK_KLINE) {
            uint32_t t0 = nowMs();
            while (nowMs() - t0 < timeoutMs) {
                PASSTHRU_MSG m; memset(&m, 0, sizeof m); uint32_t num = 1;
                if (PassThruReadMsgs(kchan, &m, &num, 50) != STATUS_NOERROR || num == 0) continue;
                if (m.RxStatus & (J_TX_MSG_TYPE | J_START_OF_MESSAGE)) continue;
                if (m.DataSize < 2) continue;
                size_t h = 1; uint8_t fmt = m.Data[0]; size_t len = fmt & 0x3F;
                if (fmt & 0x80) { if (m.DataSize < 4) continue; if (m.Data[1] != (uint8_t)rxId || m.Data[2] != (uint8_t)txId) continue; h = 3; }
                if (len == 0) { if (m.DataSize <= h) continue; len = m.Data[h++]; }
                if (len == 0 || h + len > m.DataSize) continue;
                out.assign(m.Data + h, m.Data + h + len);
                return true;
            }
            return false;
        }
        return iso.recv(out, timeoutMs);
    }
};

} // namespace pt
