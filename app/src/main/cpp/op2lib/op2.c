/* SPDX-License-Identifier: Apache-2.0
 *
 * libj2534_openport2 - a native Linux SAE J2534-1 v04.04 driver for the
 * Tactrix OpenPort 2.0 (USB VID 0403 PID CC4D).
 *
 * Transport: the OP2 exposes a USB CDC-ACM interface, which the Linux
 * cdc_acm driver binds automatically as /dev/ttyACM*. We talk the device's
 * own "at..." serial protocol over that node - no libusb, no kernel detach,
 * no udev rule needed if the user can open the tty (dialout/uucp group).
 *
 * Protocol: the wire format was recovered from the vendor DLL op20pt32.dll
 * (unpacked from memory) and cross-checked against the two open-source
 * implementations. The two details that matter most were verified by
 * disassembly of the genuine DLL:
 *
 *   - Every command carries a trailing sequence number that wraps 1..0xFFFF
 *     and is echoed back (op20pt32.dll: inc [dev+0x7c], cmp 0xffff, sprintf
 *     "at%c%d ... %u"). We use it to match each reply to its request.
 *   - Transmit timeout is J2534_Timeout * 1000 microseconds (op20pt32.dll:
 *     `imul eax, timeout, 0x3E8`), and MUST be nonzero or the firmware
 *     returns ERR_TIMEOUT without ever driving the bus. With a nonzero
 *     value it sends "att<proto> <DataSize> <TxFlags> <timeout_us> <seq>".
 *
 * See RE_NOTES.md for the full teardown.
 */
#include "j2534.h"

#include <errno.h>
#include <fcntl.h>
#ifndef OP2_ANDROID_USB
#include <glob.h>
#endif
#include <stddef.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifndef OP2_ANDROID_USB
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>
#endif
#ifdef OP2_ANDROID_USB
/* Android: the serial transport is replaced by USB bulk I/O supplied by the host app. */
extern int op2_usb_write(const void *buf, int len);          /* 1 ok / 0 fail */
extern int op2_usb_read(void *buf, int max, int timeout_ms); /* bytes, 0 timeout, <0 error */
extern int op2_usb_isopen(void);
#endif

/* J2534 ABI: 32-bit fields, as the Windows DLLs and ~/.passthru consumers use.
 * A 64-bit `unsigned long` here silently shifts every field and breaks callers. */
_Static_assert(offsetof(PASSTHRU_MSG, DataSize) == 16 && sizeof(PASSTHRU_MSG) == 24 + PM_DATA_LEN,
               "PASSTHRU_MSG must use 32-bit fields");

#define OP2_MAX_CHANNELS 3
#define OP2_RX_QUEUE     512
#define OP2_LINE_MAX     512
#define OP2_DLL_VERSION  "1.0.0"
#define OP2_API_VERSION  "04.04"

/* udev creates this per attached unit; the suffix is the USB serial:
 *   /dev/serial/by-id/usb-Tactrix_OpenPort_2.0_<serial>-if00 */
#define OP2_BYID_GLOB "/dev/serial/by-id/usb-Tactrix_OpenPort_2.0_*-if00"

/* ------------------------------------------------------------------ log */

static FILE *g_log;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void op2_log_init(void)
{
    const char *path = getenv("OP2_J2534_LOG");
    if (path && *path && !g_log)
        g_log = fopen(path, "a");
}

static __attribute__((format(printf, 1, 2))) void op2_log(const char *fmt, ...)
{
    va_list ap;
    struct timespec ts;
    if (!g_log)
        return;
    clock_gettime(CLOCK_REALTIME, &ts);
    pthread_mutex_lock(&g_log_lock);
    fprintf(g_log, "%ld.%03ld ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    pthread_mutex_unlock(&g_log_lock);
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* --------------------------------------------------------------- state */

typedef struct {
    char          type;                 /* reply type letter: 'o','e','i','r','g','f' */
    uint32_t args[6];
    int           nargs;
    uint32_t seq;
    int           has_seq;
    char          text[OP2_LINE_MAX];
} op2_reply;

typedef struct {
    int           in_use;
    uint32_t protocol;             /* J2534 protocol id, also the wire index */
    uint32_t baud, flags, slot, channel_id;
    struct { int in_use; uint32_t type, id; } filters[16];
} op2_channel;

typedef struct {
    int           fd;
    char          port[128];            /* /dev/ttyACMx */
    char          serial[64];
    pthread_mutex_t lock;
    uint32_t device_id;
    uint32_t seq;
    char          firmware[80];
    op2_channel   ch[OP2_MAX_CHANNELS];
    uint32_t next_channel_id;

    PASSTHRU_MSG  rx[OP2_RX_QUEUE];
    int           rx_head, rx_tail;

    unsigned char rbuf[4096];
    int           rbuf_len, rbuf_pos;
} op2_dev;

static op2_dev g_dev;
static int     g_init;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char    g_last_error[256] = "No error";

#define LOCK()   pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)

static long fail(long code, const char *msg)
{
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
    op2_log("-> error %ld: %s", code, msg);
    return code;
}
static long ok(void) { strcpy(g_last_error, "No error"); return STATUS_NOERROR; }
static long device_error(long code)
{
    char b[96];
    snprintf(b, sizeof(b), "device returned J2534 error %ld", code);
    return fail(code, b);
}

/* ------------------------------------------------------- serial transport */

#ifndef OP2_ANDROID_USB
/* Find the OpenPort tty. want_serial (may be NULL/empty) selects a unit by
 * its USB serial suffix. Returns TRUE and fills port/serial on success. */
static int port_find(char *port_out, size_t port_len,
                     char *serial_out, size_t serial_len,
                     const char *want_serial)
{
    glob_t g;
    int found = 0;
    size_t i;

    if (glob(OP2_BYID_GLOB, 0, NULL, &g) != 0)
        return 0;

    for (i = 0; i < g.gl_pathc; i++) {
        const char *link = g.gl_pathv[i];
        const char *base = strrchr(link, '/');
        const char *serial;
        char real[256];
        ssize_t n;

        base = base ? base + 1 : link;
        /* usb-Tactrix_OpenPort_2.0_<serial>-if00 */
        serial = base + strlen("usb-Tactrix_OpenPort_2.0_");
        {
            char sbuf[64];
            const char *dash = strstr(serial, "-if");
            size_t slen = dash ? (size_t)(dash - serial) : strlen(serial);
            if (slen >= sizeof(sbuf))
                slen = sizeof(sbuf) - 1;
            memcpy(sbuf, serial, slen);
            sbuf[slen] = '\0';

            if (want_serial && *want_serial && strcasecmp(want_serial, sbuf) != 0)
                continue;

            n = readlink(link, real, sizeof(real) - 1);
            if (n <= 0)
                continue;
            real[n] = '\0';
            /* resolve the relative ../../ttyACMx target */
            {
                const char *rb = strrchr(real, '/');
                rb = rb ? rb + 1 : real;
                snprintf(port_out, port_len, "/dev/%s", rb);
            }
            if (serial_out)
                snprintf(serial_out, serial_len, "%s", sbuf);
            found = 1;
            break;
        }
    }
    globfree(&g);
    return found;
}
#endif /* !OP2_ANDROID_USB */


static int port_open(op2_dev *d, const char *want_serial)
{
#ifdef OP2_ANDROID_USB
    (void)d; (void)want_serial;
    return op2_usb_isopen();
#else
    struct termios tio;
    int flags;

    d->fd = -1;
    if (!port_find(d->port, sizeof(d->port), d->serial, sizeof(d->serial), want_serial))
        return 0;

    d->fd = open(d->port, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (d->fd < 0)
        return 0;

    if (tcgetattr(d->fd, &tio) != 0) {
        close(d->fd); d->fd = -1; return 0;
    }
    cfmakeraw(&tio);
    cfsetispeed(&tio, B115200);         /* CDC ignores line coding, but keep it legal */
    cfsetospeed(&tio, B115200);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~CRTSCTS;
    /* Non-blocking reads: we drive timing with poll() ourselves. */
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(d->fd, TCSANOW, &tio) != 0) {
        close(d->fd); d->fd = -1; return 0;
    }

    /* Assert DTR + RTS, like the vendor DLL does on the COM port. */
    flags = TIOCM_DTR | TIOCM_RTS;
    ioctl(d->fd, TIOCMBIS, &flags);

    tcflush(d->fd, TCIOFLUSH);
    return 1;
#endif
}

static void port_close(op2_dev *d)
{
#ifdef OP2_ANDROID_USB
    (void)d;
#else
    if (d->fd >= 0) { close(d->fd); d->fd = -1; }
#endif
}
#ifdef OP2_ANDROID_USB
static int port_is_open(const op2_dev *d) { (void)d; return op2_usb_isopen(); }
#else
static int port_is_open(const op2_dev *d) { return d->fd >= 0; }
#endif

static int port_write(op2_dev *d, const void *buf, size_t len)
{
#ifdef OP2_ANDROID_USB
    (void)d;
    return op2_usb_write(buf, (int)len);
#else
    const char *p = buf;
    size_t done = 0;
    if (d->fd < 0)
        return 0;
    while (done < len) {
        ssize_t n = write(d->fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return 0;
        }
        done += (size_t)n;
    }
    return 1;
#endif
}

static void port_flush_input(op2_dev *d)
{
#ifdef OP2_ANDROID_USB
    (void)d;
#else
    if (d->fd >= 0)
        tcflush(d->fd, TCIFLUSH);
#endif
}

/* ---------------------------------------------------- stream tokenizer */

/* One byte from the read-ahead buffer; refill via a poll()+read() that blocks
 * in the kernel until data arrives or the deadline passes. Returns 1=byte,
 * 0=timeout, -1=error. */
static int rd_byte(op2_dev *d, uint32_t deadline, unsigned char *out)
{
    for (;;) {
        if (d->rbuf_pos < d->rbuf_len) {
            *out = d->rbuf[d->rbuf_pos++];
            return 1;
        }
        d->rbuf_pos = d->rbuf_len = 0;
#ifdef OP2_ANDROID_USB
        {
            int32_t remaining = (int32_t)(deadline - now_ms());
            int n;
            if (remaining < 0)
                return 0;
            n = op2_usb_read(d->rbuf, (int)sizeof(d->rbuf), (int)remaining);
            if (n < 0)
                return -1;              /* device vanished */
            if (n == 0)
                return 0;               /* timeout */
            d->rbuf_len = n;
        }
#else
        {
            struct pollfd pfd = { d->fd, POLLIN, 0 };
            int32_t remaining = (int32_t)(deadline - now_ms());
            int pr;
            if (remaining < 0)
                return 0;
            pr = poll(&pfd, 1, (int)remaining);
            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (pr == 0)
                return 0;               /* timeout */
            {
                ssize_t n = read(d->fd, d->rbuf, sizeof(d->rbuf));
                if (n < 0) {
                    if (errno == EINTR)
                        continue;
                    return -1;
                }
                if (n == 0)
                    return -1;          /* device vanished */
                d->rbuf_len = (int)n;
            }
        }
#endif
    }
}

static int rd_exact(op2_dev *d, uint32_t deadline, unsigned char *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int r = rd_byte(d, deadline, buf + got);
        if (r <= 0)
            return r;
        got++;
    }
    return 1;
}

static uint32_t be32(const unsigned char *b)
{
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

static int op2_avail(op2_dev *d)
{
    int n = 0;
#ifndef OP2_ANDROID_USB
    if (d->fd >= 0)
        ioctl(d->fd, FIONREAD, &n);
#else
    (void)d;
#endif
    return n;
}

/* Parse "ar<type> <n> <n> ... <seq>". */
static void parse_reply(const char *line, op2_reply *rep)
{
    const char *p = line;
    memset(rep, 0, sizeof(*rep));
    snprintf(rep->text, sizeof(rep->text), "%s", line);
    if (p[0] != 'a' || p[1] != 'r' || !p[2])
        return;
    rep->type = p[2];
    p += 3;
    if (*p >= '0' && *p <= '9')         /* "arg5 ..." glues proto to the type */
        rep->args[rep->nargs++] = strtoul(p, (char **)&p, 10);
    while (*p && rep->nargs < 6) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p < '0' || *p > '9')
            break;                       /* non-numeric tail e.g. "ari main ..." */
        rep->args[rep->nargs++] = strtoul(p, (char **)&p, 10);
    }
    if (rep->nargs > 0) {
        rep->seq = rep->args[rep->nargs - 1];
        rep->has_seq = 1;
    }
}

/* ------------------------------------------------------------ rx queue */

static int rx_push(op2_dev *d, const PASSTHRU_MSG *m)
{
    int next = (d->rx_head + 1) % OP2_RX_QUEUE;
    if (next == d->rx_tail)
        return 0;
    d->rx[d->rx_head] = *m;
    d->rx_head = next;
    return 1;
}
static int rx_pop(op2_dev *d, PASSTHRU_MSG *m)
{
    if (d->rx_tail == d->rx_head)
        return 0;
    *m = d->rx[d->rx_tail];
    d->rx_tail = (d->rx_tail + 1) % OP2_RX_QUEUE;
    return 1;
}
static void rx_clear(op2_dev *d)
{
    d->rx_head = d->rx_tail = 0;
    d->rbuf_pos = d->rbuf_len = 0;
}

static op2_channel *chan_by_id(op2_dev *d, uint32_t id)
{
    int i;
    for (i = 0; i < OP2_MAX_CHANNELS; i++)
        if (d->ch[i].in_use && d->ch[i].channel_id == id)
            return &d->ch[i];
    return NULL;
}
static op2_channel *chan_by_proto(op2_dev *d, uint32_t proto)
{
    int i;
    for (i = 0; i < OP2_MAX_CHANNELS; i++)
        if (d->ch[i].in_use && d->ch[i].protocol == proto)
            return &d->ch[i];
    return NULL;
}

#define TOK_NONE 0
#define TOK_REPLY 1
#define TOK_FRAME 2

/* Both output kinds begin "ar". The third byte disambiguates: a digit '1'..'6'
 * is a binary received-frame record for that protocol id (verified vs a Kvaser
 * in the op2j2534 project); a letter is an ASCII reply. Binary records are
 * self-describing by a length byte, so data bytes that spell "ar" never
 * mis-frame once we are aligned. Frame layout:
 *   'a' 'r' <digit> <len> <RxStatus:1> <Timestamp:4 BE us> <CAN-ID:4 BE> <data>
 * with len = 9 + datalen. */
static int read_token(op2_dev *d, op2_reply *rep, PASSTHRU_MSG *msg, uint32_t timeout_ms)
{
    uint32_t deadline = now_ms() + timeout_ms;
    unsigned char c;
    int r;

    for (;;) {                           /* sync to 'a''r' */
        r = rd_byte(d, deadline, &c);
        if (r <= 0) return r == 0 ? TOK_NONE : -1;
        if (c != 'a') continue;
        r = rd_byte(d, deadline, &c);
        if (r <= 0) return r == 0 ? TOK_NONE : -1;
        if (c == 'r') break;
    }

    r = rd_byte(d, deadline, &c);
    if (r <= 0) return r == 0 ? TOK_NONE : -1;

    if (c >= '1' && c <= '6') {
        unsigned char len, payload[OP2_LINE_MAX];
        uint32_t proto = (uint32_t)(c - '0');
        if (rd_byte(d, deadline, &len) <= 0) return TOK_NONE;
        if (len < 9) return TOK_NONE;  /* len is u8, payload holds 512 */
        if (rd_exact(d, deadline, payload, len) <= 0) return TOK_NONE;
        if (msg) {
            uint32_t datalen = (uint32_t)len - 9;
            memset(msg, 0, sizeof(*msg));
            msg->ProtocolID = proto;
            msg->RxStatus   = payload[0];
            msg->Timestamp  = be32(payload + 1);
            msg->DataSize   = datalen + 4;        /* 4-byte CAN ID + data */
            if (msg->DataSize > PM_DATA_LEN) msg->DataSize = PM_DATA_LEN;
            memcpy(msg->Data, payload + 5, msg->DataSize);
        }
        return TOK_FRAME;
    }

    {                                    /* ASCII reply "ar<c>...\r\n" */
        char line[OP2_LINE_MAX];
        size_t used = 0;
        line[used++] = 'a'; line[used++] = 'r'; line[used++] = (char)c;
        for (;;) {
            unsigned char x;
            r = rd_byte(d, deadline, &x);
            if (r <= 0) return TOK_NONE;
            if (x == '\n') break;
            if (used + 1 < sizeof(line)) line[used++] = (char)x;
        }
        while (used && line[used - 1] == '\r') used--;
        line[used] = '\0';
        if (rep) parse_reply(line, rep);
        return TOK_REPLY;
    }
}

static void op2_pump(op2_dev *d)
{
    if (!port_is_open(d))
        return;
    while (d->rbuf_pos < d->rbuf_len || op2_avail(d) > 0) {
        PASSTHRU_MSG m;
        op2_reply rep;
        int t = read_token(d, &rep, &m, 20);
        if (t == TOK_FRAME) {
            if (!rx_push(d, &m)) op2_log("RX queue full, dropped frame");
        } else if (t == TOK_REPLY) {
            op2_log("RX async reply: %s", rep.text);
        } else {
            break;
        }
    }
}

static int wait_rx(op2_dev *d, PASSTHRU_MSG *m, uint32_t timeout_ms)
{
    uint32_t deadline = now_ms() + timeout_ms;
    if (!port_is_open(d))
        return 0;
    for (;;) {
        op2_reply rep;
        int32_t remaining = (int32_t)(deadline - now_ms());
        int t;
        if (remaining < 0) return 0;
        t = read_token(d, &rep, m, (uint32_t)remaining);
        if (t == TOK_FRAME) return 1;
        if (t < 0) return 0;
    }
}

/* ------------------------------------------------------------- commands */

/* Send a command (trailing seq appended and echoed back) plus an optional raw
 * body after the CRLF, and wait for the matching reply. Returns a J2534 code. */
static __attribute__((format(printf, 6, 7))) long op2_command(op2_dev *d, op2_reply *rep, uint32_t timeout_ms,
                        const void *body, size_t body_len, const char *fmt, ...)
{
    char cmd[OP2_LINE_MAX];
    uint32_t seq;
    int n;
    va_list ap;
    uint32_t deadline;
    op2_reply local;
    PASSTHRU_MSG frame;

    if (!rep) rep = &local;
    if (!port_is_open(d)) return ERR_DEVICE_NOT_CONNECTED;

    seq = ++d->seq;
    if (seq == 0 || seq > 0xFFFF) { d->seq = 1; seq = 1; }   /* mirror DLL wrap */

    va_start(ap, fmt);
    n = vsnprintf(cmd, sizeof(cmd) - 16, fmt, ap);
    va_end(ap);
    if (n < 0) return ERR_FAILED;
    n += snprintf(cmd + n, sizeof(cmd) - n - 1, " %u\r\n", seq);

    op2_log("TX: %.*s%s", n - 2, cmd, body_len ? " (+payload)" : "");
    if (!port_write(d, cmd, (size_t)n)) return ERR_DEVICE_NOT_CONNECTED;
    if (body && body_len && !port_write(d, body, body_len)) return ERR_DEVICE_NOT_CONNECTED;

    deadline = now_ms() + timeout_ms;
    for (;;) {
        int32_t remaining = (int32_t)(deadline - now_ms());
        int t;
        if (remaining <= 0) { op2_log("timeout waiting for seq %u", seq); return ERR_TIMEOUT; }
        t = read_token(d, rep, &frame, (uint32_t)remaining);
        if (t == TOK_FRAME) {            /* bus frame arrived mid-wait: queue it */
            if (!rx_push(d, &frame)) op2_log("RX queue full, dropped frame");
            continue;
        }
        if (t != TOK_REPLY) continue;
        op2_log("RX: %s", rep->text);
        if (rep->type == 'i') return STATUS_NOERROR;     /* 'ari' has no seq */
        if (rep->has_seq && rep->seq != seq) continue;   /* stale reply */
        if (rep->type == 'e')
            return (rep->nargs >= 2) ? (long)rep->args[0] : (long)ERR_FAILED;
        return STATUS_NOERROR;
    }
}

static int op2_connect(op2_dev *d, const char *want_serial)
{
    op2_reply rep;
    if (!port_open(d, want_serial))
        return 0;
    /* Clear any half-command from a crashed session, then check it answers. */
    port_write(d, "\r\n", 2);
    usleep(60000);
    port_flush_input(d);
    if (op2_command(d, &rep, 1500, NULL, 0, "ati") != STATUS_NOERROR || rep.type != 'i') {
        port_close(d);
        return 0;
    }
    snprintf(d->firmware, sizeof(d->firmware), "%s", rep.text);
    op2_log("connected on %s (%s): %s", d->port, d->serial, d->firmware);
    op2_command(d, NULL, 1500, NULL, 0, "ata");   /* release channels a prior session left */
    rx_clear(d);
    return 1;
}

static void op2_shutdown(op2_dev *d)
{
    if (port_is_open(d)) {
        op2_command(d, NULL, 800, NULL, 0, "ata");
        port_close(d);
    }
    memset(d->ch, 0, sizeof(d->ch));
    rx_clear(d);
}

/* ===================================================================== API */

int32_t PassThruOpen(const void *pName, uint32_t *pDeviceID)
{
    const char *want = (const char *)pName;
    long rc;
    LOCK();
    op2_log_init();
    op2_log("PassThruOpen(name=%s)", want && *want ? want : "(null)");
    if (!pDeviceID) { rc = fail(ERR_NULL_PARAMETER, "pDeviceID is NULL"); UNLOCK(); return rc; }
    if (g_init)     { rc = fail(ERR_DEVICE_IN_USE, "device already open"); UNLOCK(); return rc; }

    memset(&g_dev, 0, sizeof(g_dev));
    g_dev.fd = -1;
    g_dev.next_channel_id = 1;
    if (!op2_connect(&g_dev, want)) {
        rc = fail(ERR_DEVICE_NOT_CONNECTED,
                  "no OpenPort 2.0 found (expected " OP2_BYID_GLOB ")");
        UNLOCK(); return rc;
    }
    g_dev.device_id = 1;
    g_init = 1;
    *pDeviceID = g_dev.device_id;
    rc = ok();
    UNLOCK();
    return rc;
}

int32_t PassThruClose(uint32_t DeviceID)
{
    long rc;
    LOCK();
    op2_log("PassThruClose(%u)", DeviceID);
    if (!g_init || DeviceID != g_dev.device_id) { rc = fail(ERR_INVALID_DEVICE_ID, "invalid device ID"); UNLOCK(); return rc; }
    op2_shutdown(&g_dev);
    g_init = 0;
    rc = ok();
    UNLOCK();
    return rc;
}

int32_t PassThruConnect(uint32_t DeviceID, uint32_t ProtocolID,
                        uint32_t Flags, uint32_t Baudrate,
                        uint32_t *pChannelID)
{
    op2_channel *c = NULL;
    long rc;
    int slot;
    LOCK();
    op2_log("PassThruConnect(dev=%u proto=%u flags=0x%08X baud=%u)",
            DeviceID, ProtocolID, Flags, Baudrate);
    if (!g_init || DeviceID != g_dev.device_id) { rc = fail(ERR_INVALID_DEVICE_ID, "invalid device ID"); UNLOCK(); return rc; }
    if (!pChannelID) { rc = fail(ERR_NULL_PARAMETER, "pChannelID is NULL"); UNLOCK(); return rc; }

    switch (ProtocolID) {
    case ISO9141: case ISO14230: case CAN: case ISO15765: break;
    case J1850VPW: case J1850PWM:
        rc = fail(ERR_INVALID_PROTOCOL_ID, "J1850 not supported by this OpenPort firmware");
        UNLOCK(); return rc;
    default:
        rc = fail(ERR_INVALID_PROTOCOL_ID, "unsupported protocol");
        UNLOCK(); return rc;
    }
    if (ProtocolID == ISO14230 && Baudrate != 10400) {
        rc = fail(ERR_INVALID_BAUDRATE, "ISO14230 supports 10400 baud only"); UNLOCK(); return rc;
    }
    if (chan_by_proto(&g_dev, ProtocolID)) {
        rc = fail(ERR_CHANNEL_IN_USE, "a channel for this protocol is open"); UNLOCK(); return rc;
    }
    for (slot = 0; slot < OP2_MAX_CHANNELS; slot++)
        if (!g_dev.ch[slot].in_use) { c = &g_dev.ch[slot]; break; }
    if (!c) { rc = fail(ERR_EXCEEDED_LIMIT, "all three channels are in use"); UNLOCK(); return rc; }

    /* ato<proto> <flags> <baud> <channel> <seq> - Flags before Baudrate,
     * matching PassThruConnect and the genuine DLL (verified by disassembly). */
    rc = op2_command(&g_dev, NULL, 3000, NULL, 0, "ato%u %u %u %d",
                     ProtocolID, Flags, Baudrate, slot);
    if (rc != STATUS_NOERROR) { rc = device_error(rc); UNLOCK(); return rc; }

    memset(c, 0, sizeof(*c));
    c->in_use = 1;
    c->protocol = ProtocolID;
    c->baud = Baudrate;
    c->flags = Flags;
    c->slot = (uint32_t)slot;
    c->channel_id = g_dev.next_channel_id++;
    *pChannelID = c->channel_id;
    op2_log("PassThruConnect -> channelId=%u (slot %d)", c->channel_id, slot);
    rc = ok();
    UNLOCK();
    return rc;
}

int32_t PassThruDisconnect(uint32_t ChannelID)
{
    op2_channel *c;
    long rc;
    LOCK();
    op2_log("PassThruDisconnect(%u)", ChannelID);
    if (!g_init) { rc = fail(ERR_DEVICE_NOT_CONNECTED, "device not open"); UNLOCK(); return rc; }
    c = chan_by_id(&g_dev, ChannelID);
    if (!c) { rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID"); UNLOCK(); return rc; }
    op2_command(&g_dev, NULL, 2000, NULL, 0, "atc%u", c->protocol);
    memset(c, 0, sizeof(*c));
    rc = ok();
    UNLOCK();
    return rc;
}

int32_t PassThruReadMsgs(uint32_t ChannelID, PASSTHRU_MSG *pMsg,
                         uint32_t *pNumMsgs, uint32_t Timeout)
{
    op2_channel *c;
    uint32_t want, got = 0;
    uint32_t deadline;
    long rc;
    LOCK();
    if (!g_init) { rc = fail(ERR_DEVICE_NOT_CONNECTED, "device not open"); UNLOCK(); return rc; }
    c = chan_by_id(&g_dev, ChannelID);
    if (!c) { rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID"); UNLOCK(); return rc; }
    if (!pMsg || !pNumMsgs) { rc = fail(ERR_NULL_PARAMETER, "NULL parameter"); UNLOCK(); return rc; }

    want = *pNumMsgs;
    *pNumMsgs = 0;
    deadline = now_ms() + Timeout;

    {   /* hand back anything already queued */
        PASSTHRU_MSG m;
        op2_pump(&g_dev);
        while (got < want && rx_pop(&g_dev, &m))
            if (m.ProtocolID == c->protocol) pMsg[got++] = m;
    }
    while (got < want && Timeout != 0) {
        PASSTHRU_MSG m;
        int32_t remaining = (int32_t)(deadline - now_ms());
        if (remaining <= 0) break;
        if (!wait_rx(&g_dev, &m, (uint32_t)remaining)) break;
        if (m.ProtocolID == c->protocol) pMsg[got++] = m;
    }

    *pNumMsgs = got;
    if (got == 0) { rc = fail(ERR_BUFFER_EMPTY, "no messages available"); UNLOCK(); return rc; }
    rc = ok();
    UNLOCK();
    return rc;
}

int32_t PassThruWriteMsgs(uint32_t ChannelID, const PASSTHRU_MSG *pMsg,
                          uint32_t *pNumMsgs, uint32_t Timeout)
{
    op2_channel *c;
    uint32_t i, sent = 0;
    long rc = STATUS_NOERROR;
    LOCK();
    if (!g_init) { rc = fail(ERR_DEVICE_NOT_CONNECTED, "device not open"); UNLOCK(); return rc; }
    c = chan_by_id(&g_dev, ChannelID);
    if (!c) { rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID"); UNLOCK(); return rc; }
    if (!pMsg || !pNumMsgs) { rc = fail(ERR_NULL_PARAMETER, "NULL parameter"); UNLOCK(); return rc; }

    for (i = 0; i < *pNumMsgs; i++) {
        const PASSTHRU_MSG *m = &pMsg[i];
        uint32_t tx_timeout_us;
        if (m->DataSize == 0 || m->DataSize > PM_DATA_LEN) { rc = fail(ERR_INVALID_MSG, "DataSize out of range"); break; }
        if (m->ProtocolID != c->protocol) { rc = fail(ERR_MSG_PROTOCOL_ID, "message protocol != channel"); break; }

        /* att<proto> <DataSize> <TxFlags> <tx_timeout_us> <seq> + raw Data.
         * timeout is J2534 Timeout*1000 us and MUST be nonzero (DLL-verified);
         * floor a caller's 0 so the frame still transmits. */
        tx_timeout_us = Timeout ? (Timeout * 1000UL) : 100000UL;
        rc = op2_command(&g_dev, NULL, (Timeout ? Timeout : 1000) + 500,
                         m->Data, m->DataSize, "att%u %u %u %u",
                         c->protocol, m->DataSize, m->TxFlags, tx_timeout_us);
        if (rc != STATUS_NOERROR) { device_error(rc); break; }
        sent++;
    }
    *pNumMsgs = sent;
    if (sent && rc == STATUS_NOERROR) rc = ok();
    UNLOCK();
    return rc;
}

int32_t PassThruStartPeriodicMsg(uint32_t ChannelID, const PASSTHRU_MSG *pMsg,
                                 uint32_t *pMsgID, uint32_t TimeInterval)
{
    (void)ChannelID; (void)pMsg; (void)pMsgID; (void)TimeInterval;
    /* 'atm' exists but its argument layout is unconfirmed; report honestly
     * rather than silently feeding nothing onto a bus the caller trusts. */
    return fail(ERR_NOT_SUPPORTED, "periodic messages not implemented");
}
int32_t PassThruStopPeriodicMsg(uint32_t ChannelID, uint32_t MsgID)
{
    (void)ChannelID; (void)MsgID;
    return fail(ERR_NOT_SUPPORTED, "periodic messages not implemented");
}

int32_t PassThruStartMsgFilter(uint32_t ChannelID, uint32_t FilterType,
                               const PASSTHRU_MSG *pMaskMsg, const PASSTHRU_MSG *pPatternMsg,
                               const PASSTHRU_MSG *pFlowControlMsg, uint32_t *pFilterID)
{
    op2_channel *c;
    long rc;
    int i;
    LOCK();
    if (!g_init) { rc = fail(ERR_DEVICE_NOT_CONNECTED, "device not open"); UNLOCK(); return rc; }
    c = chan_by_id(&g_dev, ChannelID);
    if (!c) { rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID"); UNLOCK(); return rc; }
    if (!pFilterID) { rc = fail(ERR_NULL_PARAMETER, "pFilterID is NULL"); UNLOCK(); return rc; }
    if (FilterType != PASS_FILTER && FilterType != BLOCK_FILTER && FilterType != FLOW_CONTROL_FILTER) {
        rc = fail(ERR_INVALID_FILTER_ID, "unknown filter type"); UNLOCK(); return rc;
    }
    if (FilterType == FLOW_CONTROL_FILTER && !pFlowControlMsg) {
        rc = fail(ERR_NULL_PARAMETER, "flow control filter needs pFlowControlMsg"); UNLOCK(); return rc;
    }
    if (!pMaskMsg || !pPatternMsg) { rc = fail(ERR_NULL_PARAMETER, "filter needs mask and pattern"); UNLOCK(); return rc; }
    for (i = 0; i < 16; i++) if (!c->filters[i].in_use) break;
    if (i == 16) { rc = fail(ERR_EXCEEDED_LIMIT, "filter table full"); UNLOCK(); return rc; }

    /* atf<proto> <type> <mask_len> <pattern_len> <seq> + <mask><pattern>.
     * For CAN each selector is a 4-byte big-endian message-ID. Installing a
     * filter is REQUIRED before the device forwards any received frame. */
    {
        unsigned char payload[8];
        uint32_t mask = 0, patt = 0;
        op2_reply rep;
        if (pMaskMsg->DataSize >= 4)
            mask = be32(pMaskMsg->Data);
        if (pPatternMsg->DataSize >= 4)
            patt = be32(pPatternMsg->Data);
        payload[0]=(unsigned char)(mask>>24); payload[1]=(unsigned char)(mask>>16);
        payload[2]=(unsigned char)(mask>>8);  payload[3]=(unsigned char)mask;
        payload[4]=(unsigned char)(patt>>24); payload[5]=(unsigned char)(patt>>16);
        payload[6]=(unsigned char)(patt>>8);  payload[7]=(unsigned char)patt;

        rc = op2_command(&g_dev, &rep, 2000, payload, sizeof(payload),
                         "atf%u %u 4 4", c->protocol, FilterType);
        if (rc != STATUS_NOERROR) { rc = device_error(rc); UNLOCK(); return rc; }
        c->filters[i].in_use = 1;
        c->filters[i].type = FilterType;
        c->filters[i].id = (rep.nargs >= 3) ? rep.args[1] : (uint32_t)(i + 1);
        *pFilterID = c->filters[i].id;
    }
    op2_log("StartMsgFilter type=%u -> id=%u", FilterType, *pFilterID);
    rc = ok();
    UNLOCK();
    return rc;
}

int32_t PassThruStopMsgFilter(uint32_t ChannelID, uint32_t FilterID)
{
    op2_channel *c;
    long rc;
    int i;
    LOCK();
    if (!g_init) { rc = fail(ERR_DEVICE_NOT_CONNECTED, "device not open"); UNLOCK(); return rc; }
    c = chan_by_id(&g_dev, ChannelID);
    if (!c) { rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID"); UNLOCK(); return rc; }
    for (i = 0; i < 16; i++)
        if (c->filters[i].in_use && c->filters[i].id == FilterID) {
            c->filters[i].in_use = 0;
            rc = ok(); UNLOCK(); return rc;
        }
    rc = fail(ERR_INVALID_FILTER_ID, "unknown filter ID");
    UNLOCK();
    return rc;
}

int32_t PassThruSetProgrammingVoltage(uint32_t DeviceID, uint32_t PinNumber,
                                      uint32_t Voltage)
{
    long rc;
    LOCK();
    op2_log("PassThruSetProgrammingVoltage(pin=%u v=%u)", PinNumber, Voltage);
    if (!g_init || DeviceID != g_dev.device_id) { rc = fail(ERR_INVALID_DEVICE_ID, "invalid device ID"); UNLOCK(); return rc; }
    rc = op2_command(&g_dev, NULL, 2000, NULL, 0, "atp %u %u", PinNumber, Voltage);
    if (rc != STATUS_NOERROR) { rc = device_error(rc); UNLOCK(); return rc; }
    rc = ok();
    UNLOCK();
    return rc;
}

int32_t PassThruReadVersion(uint32_t DeviceID, char *pFirmwareVersion,
                            char *pDllVersion, char *pApiVersion)
{
    long rc;
    LOCK();
    if (!g_init || DeviceID != g_dev.device_id) { rc = fail(ERR_INVALID_DEVICE_ID, "invalid device ID"); UNLOCK(); return rc; }
    if (!pFirmwareVersion || !pDllVersion || !pApiVersion) { rc = fail(ERR_NULL_PARAMETER, "NULL version buffer"); UNLOCK(); return rc; }
    {   /* "ari main code version : 1.17.4877" -> "1.17.4877" */
        const char *v = strrchr(g_dev.firmware, ':');
        v = v ? v + 1 : g_dev.firmware;
        while (*v == ' ') v++;
        snprintf(pFirmwareVersion, 80, "%s", v);
    }
    strcpy(pDllVersion, OP2_DLL_VERSION);
    strcpy(pApiVersion, OP2_API_VERSION);
    rc = ok();
    UNLOCK();
    return rc;
}

int32_t PassThruGetLastError(char *pErrorDescription)
{
    if (!pErrorDescription)
        return ERR_NULL_PARAMETER;
    snprintf(pErrorDescription, 80, "%s", g_last_error);
    return STATUS_NOERROR;
}

int32_t PassThruIoctl(uint32_t ChannelID, uint32_t IoctlID,
                      const void *pInput, void *pOutput)
{
    op2_channel *c;
    long rc;
    LOCK();
    op2_log("PassThruIoctl(ch=%u id=%u)", ChannelID, IoctlID);
    if (!g_init) { rc = fail(ERR_DEVICE_NOT_CONNECTED, "device not open"); UNLOCK(); return rc; }

    switch (IoctlID) {
    case READ_VBATT: {
        op2_reply rep;
        if (!pOutput) { rc = fail(ERR_NULL_PARAMETER, "pOutput is NULL"); break; }
        rc = op2_command(&g_dev, &rep, 2000, NULL, 0, "atr 16");   /* pin 16 = battery */
        if (rc != STATUS_NOERROR) { rc = device_error(rc); break; }
        if (rep.type != 'r' || rep.nargs < 3) { rc = fail(ERR_FAILED, "malformed voltage reply"); break; }
        *(uint32_t *)pOutput = rep.args[1];
        rc = ok();
        break;
    }
    case READ_PROG_VOLTAGE: {
        op2_reply rep;
        if (!pOutput) { rc = fail(ERR_NULL_PARAMETER, "pOutput is NULL"); break; }
        rc = op2_command(&g_dev, &rep, 2000, NULL, 0, "atr 17");
        if (rc != STATUS_NOERROR) { rc = device_error(rc); break; }
        if (rep.type != 'r' || rep.nargs < 3) { rc = fail(ERR_FAILED, "malformed voltage reply"); break; }
        *(uint32_t *)pOutput = rep.args[1];
        rc = ok();
        break;
    }
    case SET_CONFIG: {
        const SCONFIG_LIST *list = pInput;
        uint32_t i;
        c = chan_by_id(&g_dev, ChannelID);
        if (!c) { rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID"); break; }
        if (!list || (list->NumOfParams && !list->ConfigPtr)) { rc = fail(ERR_NULL_PARAMETER, "bad SCONFIG_LIST"); break; }
        rc = STATUS_NOERROR;
        for (i = 0; i < list->NumOfParams; i++) {
            rc = op2_command(&g_dev, NULL, 2000, NULL, 0, "ats%u %u %u",
                             c->protocol, list->ConfigPtr[i].Parameter, list->ConfigPtr[i].Value);
            if (rc != STATUS_NOERROR) { rc = device_error(rc); break; }
        }
        if (rc == STATUS_NOERROR) rc = ok();
        break;
    }
    case GET_CONFIG: {
        const SCONFIG_LIST *list = pInput;
        uint32_t i;
        c = chan_by_id(&g_dev, ChannelID);
        if (!c) { rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID"); break; }
        if (!list || (list->NumOfParams && !list->ConfigPtr)) { rc = fail(ERR_NULL_PARAMETER, "bad SCONFIG_LIST"); break; }
        rc = STATUS_NOERROR;
        for (i = 0; i < list->NumOfParams; i++) {
            op2_reply rep;
            rc = op2_command(&g_dev, &rep, 2000, NULL, 0, "atg%u %u",
                             c->protocol, list->ConfigPtr[i].Parameter);
            if (rc != STATUS_NOERROR) { rc = device_error(rc); break; }
            if (rep.nargs < 4) { rc = fail(ERR_FAILED, "malformed config reply"); break; }
            list->ConfigPtr[i].Value = rep.args[2];
        }
        if (rc == STATUS_NOERROR) rc = ok();
        break;
    }
    case CLEAR_RX_BUFFER:
        rx_clear(&g_dev);
        port_flush_input(&g_dev);
        rc = ok();
        break;
    case CLEAR_TX_BUFFER:
    case CLEAR_PERIODIC_MSGS:
        rc = ok();
        break;
    case CLEAR_MSG_FILTERS: {
        int i;
        c = chan_by_id(&g_dev, ChannelID);
        if (!c) { rc = fail(ERR_INVALID_CHANNEL_ID, "invalid channel ID"); break; }
        for (i = 0; i < 16; i++) c->filters[i].in_use = 0;
        rc = ok();
        break;
    }
    case FAST_INIT:
    case FIVE_BAUD_INIT:
        rc = fail(ERR_NOT_SUPPORTED, "K-line init routines not implemented");
        break;
    default:
        rc = fail(ERR_INVALID_IOCTL_ID, "unknown ioctl ID");
        break;
    }
    UNLOCK();
    return rc;
}
