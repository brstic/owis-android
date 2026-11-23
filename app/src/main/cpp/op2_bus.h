// Tactrix OpenPort 2.0 transport for OWIS, over a raw USB byte pipe (Android: UsbDeviceConnection
// bulk transfers on the CDC-data interface, EP 0x02 OUT / 0x82 IN). The OpenPort 2.0 speaks an ASCII
// command protocol ("ato/atf/att ... <seq>\r\n") and returns raw CAN frames as binary records; ISO-TP
// (ISO 15765-2) and VW TP 2.0 run in user space on top, exactly as for an SLCAN adapter.
//
// Protocol (reverse-engineered, see github.com/roffe/libj2534_openport2):
//   connect : write "\r\n", 60 ms, flush; "ati <seq>\r\n" -> "ari main code version : ..."; "ata <seq>\r\n"
//   open CAN: "ato5 0 500000 <slot> <seq>\r\n"        -> "aro <seq>"          (proto 5 = CAN, 500 kbit/s)
//   filter  : "atf5 1 4 4 <seq>\r\n" + 8 bytes mask|pattern (BE32 each)  -> "arf5 <id> <seq>"
//   transmit: "att5 <len> <txflags> <tmo_us> <seq>\r\n" + <id BE32><data>   -> "aro <seq>"
//   rx frame: 'a' 'r' <proto '1'..'6'> <len u8> <RxStatus> <TS BE32> <CAN-ID BE32> <data...>, len = 9 + datalen
#pragma once
#include "engine/bus.h"
#include "engine/isotp.h"
#include <deque>
#include <functional>

namespace pt {

// A raw byte pipe to the adapter. write() returns bytes written (<0 on error); read() returns bytes read
// into buf (0 on timeout, <0 on error). The platform supplies these (Android bulkTransfer via JNI).
struct UsbPipe {
    std::function<int(const uint8_t *, int)> write;
    std::function<int(uint8_t *, int, int)> read;      // buf, max, timeoutMs
};

class Op2Bus : public Bus, public RawCan {
    UsbPipe usb;
    IsoTp iso{*this};
    Tp20 tp{*this};
    std::vector<uint8_t> rx;                 // bytes read from USB, not yet parsed
    std::deque<CanFrame> frames;             // parsed inbound CAN frames
    unsigned seq = 1;
    int slot = 0;
    bool chan = false;
    // last ASCII reply seen
    struct Reply { char type = 0; unsigned seq = 0; long a0 = 0, a1 = 0; bool valid = false, ident = false; };
    Reply lastReply;

    unsigned nextSeq() { unsigned s = seq; seq = (seq >= 0xFFFF) ? 1 : seq + 1; return s; }

    bool writeAll(const uint8_t *d, int n) {
        int off = 0;
        while (off < n) { int w = usb.write(d + off, n - off); if (w <= 0) return false; off += w; }
        return true;
    }
    // Send "<cmd> <seq>\r\n" followed by an optional raw body. Returns the seq used (0 on write error).
    unsigned command(const std::string &cmd, const uint8_t *body = nullptr, int bodyLen = 0) {
        unsigned s = nextSeq();
        char head[64]; int n = snprintf(head, sizeof head, "%s %u\r\n", cmd.c_str(), s);
        if (tap) tap('T', txId, std::vector<uint8_t>(head, head + n));
        if (!writeAll((const uint8_t *)head, n)) return 0;
        if (body && bodyLen && !writeAll(body, bodyLen)) return 0;
        return s;
    }
    // Pull one chunk from USB into rx. Returns false on timeout/error.
    bool fill(int timeoutMs) {
        uint8_t buf[512];
        int r = usb.read(buf, sizeof buf, timeoutMs);
        if (r <= 0) return false;
        rx.insert(rx.end(), buf, buf + r);
        return true;
    }
    // Parse rx: queue CAN frames, record the last ASCII reply. Returns true if anything was parsed.
    bool parse() {
        bool any = false;
        size_t i = 0;
        while (i + 1 < rx.size()) {
            if (!(rx[i] == 'a' && rx[i + 1] == 'r')) { i++; continue; }       // resync to "ar"
            if (i + 2 >= rx.size()) break;
            uint8_t c = rx[i + 2];
            if (c >= '1' && c <= '6') {                                       // binary CAN frame
                if (i + 3 >= rx.size()) break;
                int len = rx[i + 3];
                if (i + 4 + len > (int)rx.size()) break;                      // frame not fully arrived yet
                const uint8_t *p = &rx[i + 4];                                // RxStatus TS(4) ID(4) data...
                uint8_t rxStatus = p[0];
                if (len >= 9 && !(rxStatus & 0x10) && !(rxStatus & 0x20)) {   // skip TX-done / loopback echoes
                    CanFrame f;
                    f.id = ((uint32_t)p[5] << 24) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 8) | p[8];
                    f.d.assign(p + 9, p + len);
                    if (tap) tap('R', f.id, f.d);
                    frames.push_back(std::move(f));
                }
                i += 4 + len; any = true; continue;
            }
            // ASCII reply: "ar<type> <args...>\r\n"
            size_t e = i + 2;
            while (e + 1 < rx.size() && !(rx[e] == '\r' && rx[e + 1] == '\n')) e++;
            if (e + 1 >= rx.size()) break;                                    // line not complete yet
            std::string line((const char *)&rx[i], e - i);
            parseReply(line);
            i = e + 2; any = true;
        }
        if (i > 0) rx.erase(rx.begin(), rx.begin() + i);
        return any;
    }
    void parseReply(const std::string &line) {
        // line like "aro 5", "are 10 7", "arf5 1 2", "ari main code version : 1.17.4877", "arg5 1 500000 3"
        Reply rp; rp.type = line.size() > 2 ? line[2] : 0;
        if (rp.type == 'i') { rp.ident = true; rp.valid = true; lastReply = rp; return; }
        // collect the trailing integers
        std::vector<long> nums; const char *s = line.c_str();
        for (const char *q = s; *q; q++) {
            if ((*q >= '0' && *q <= '9') && (q == s || !(q[-1] >= '0' && q[-1] <= '9'))) nums.push_back(strtol(q, nullptr, 10));
        }
        // the sequence is always the last integer
        if (!nums.empty()) { rp.seq = (unsigned)nums.back();
            if (nums.size() >= 2) rp.a0 = nums[nums.size() - 2];
            if (nums.size() >= 3) rp.a1 = nums[nums.size() - 3]; }
        rp.valid = true; lastReply = rp;
    }
    // Wait for an "aro"/"are"/"arf" reply to a given seq. Returns true on "aro"/"arf".
    bool ack(unsigned s, int timeoutMs, long *filterId = nullptr) {
        if (s == 0) return false;
        uint32_t t0 = nowMs();
        for (;;) {
            parse();
            if (lastReply.valid && lastReply.seq == s && (lastReply.type == 'o' || lastReply.type == 'e' || lastReply.type == 'f')) {
                bool ok = lastReply.type != 'e';
                if (ok && filterId && lastReply.type == 'f') *filterId = lastReply.a0;
                lastReply.valid = false;
                return ok;
            }
            int left = (int)timeoutMs - (int)(nowMs() - t0);
            if (left <= 0) return false;
            fill(left);
        }
    }
    bool waitIdent(int timeoutMs) {
        uint32_t t0 = nowMs();
        for (;;) {
            parse();
            if (lastReply.valid && lastReply.ident) { lastReply.valid = false; return true; }
            int left = (int)timeoutMs - (int)(nowMs() - t0);
            if (left <= 0) return false;
            fill(left);
        }
    }

public:
    explicit Op2Bus(const UsbPipe &pipe) : usb(pipe) {}
    ~Op2Bus() override { close(); }

    bool open() override {
        rx.clear(); frames.clear(); seq = 1;
        uint8_t crlf[2] = {'\r', '\n'}; writeAll(crlf, 2);
        sleepMs(60);
        while (fill(20)) {}                      // flush whatever the device buffered
        rx.clear();
        if (!ackIdent()) { error("[ERROR] the OpenPort 2.0 did not answer (cable and ignition on?)."); return false; }
        command("ata");                          // release any channels left open; ignore the ack
        ack(seq - 1, 300);
        unsigned s = command("ato5 0 500000 " + std::to_string(slot));
        if (!ack(s, 1500)) { error("[ERROR] the adapter did not open the CAN channel."); return false; }
        uint8_t filt[8] = {0, 0, 0, 0, 0, 0, 0, 0};              // pass-all: mask 0, pattern 0
        s = command("atf5 1 4 4", filt, 8);
        ack(s, 1000);
        chan = true;
        return true;
    }
    bool ackIdent() {
        unsigned s = command("ati");
        (void)s;
        return waitIdent(1500);
    }
    void close() override {
        if (tp.connected()) tp.close();
        if (chan) { command("atc5"); ack(seq - 1, 300); chan = false; }
        command("ata"); ack(seq - 1, 300);
    }

    // ---- RawCan ----
    bool rawOpen() override { return chan; }                    // the single CAN channel also carries TP 2.0
    void rawClose() override {}
    bool sendFrame(const CanFrame &f) override {
        bool ext = (f.id & 0x80000000u) || (f.id & 0x1FFFFFFF) > 0x7FF;
        uint32_t id = f.id & 0x1FFFFFFF;
        std::vector<uint8_t> body = {(uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id};
        for (size_t k = 0; k < f.d.size() && k < 8; k++) body.push_back(f.d[k]);
        unsigned txflags = ext ? 0x100u : 0u;                   // CAN_29BIT_ID
        unsigned s = command("att5 " + std::to_string(body.size()) + " " + std::to_string(txflags) + " 100000", body.data(), (int)body.size());
        return ack(s, 300);                                     // device ACKs the queued frame with "aro"
    }
    bool recvFrame(CanFrame &f, unsigned timeoutMs) override {
        uint32_t t0 = nowMs();
        for (;;) {
            if (!frames.empty()) { f = frames.front(); frames.pop_front(); return true; }
            parse();
            if (!frames.empty()) continue;
            int left = (int)timeoutMs - (int)(nowMs() - t0);
            if (left <= 0) return false;
            fill(left);
        }
    }

    // ---- Bus (ISO-TP / TP 2.0 on top of the frames) ----
    bool target(uint32_t tx, uint32_t rx_, int linkKind = LINK_CAN) override {
        if (tp.connected() && (linkKind != LINK_TP20 || tx != tp.address())) tp.close();
        Bus::target(tx, rx_, linkKind);
        if (linkKind == LINK_TP20) { if (!tp.connected()) tp.open((uint8_t)tx); }
        else iso.setIds(tx, rx_);
        return chan;
    }
    bool sendRaw(const std::vector<uint8_t> &p) override {
        if (kind == LINK_TP20) { tp.keepAlive(); return tp.sendMessage(p); }
        if (kind == LINK_KLINE) return false;                   // OpenPort 2.0 K-line init not supported here
        return iso.send(p);
    }
    bool recvRaw(std::vector<uint8_t> &out, unsigned timeoutMs) override {
        if (kind == LINK_TP20) return tp.recvMessage(out, timeoutMs);
        if (kind == LINK_KLINE) return false;
        return iso.recv(out, timeoutMs);
    }
};

} // namespace pt
