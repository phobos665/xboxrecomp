/**
 * The console's network card, as far as System Link needs it.
 *
 * System Link is plain Ethernet, and the network stack that speaks it (XNet)
 * is linked into every title and drives the card's registers itself. So the
 * card is the boundary: below it a frame is a byte array, above it is the
 * title's own lifted code. docs/technical/system-link.md has the plan and
 * what was measured.
 *
 * Three pieces live here.
 *
 * The Ethernet address. XNet builds a console's network identity (XNADDR)
 * from it, and two consoles with one address cannot share a game -- which is
 * what every install used to be, at 00:50:F2:00:00:01. Each save folder now
 * gets its own, chosen at random under Microsoft's 00:50:F2 prefix the first
 * time a title asks and kept in console.ini beside the partition images. The
 * save folder because that is the console's disk: an identity belongs with
 * the disk it was made with (Xbox Live will need them paired), and it means
 * a scripted run under RECOMP_SAVE_DIR gets an identity of its own without
 * touching the player's.
 *
 *     RECOMP_SYSLINK_MAC=00:50:F2:12:34:56   this address, not the saved one
 *
 * The register log. Under RECOMP_NIC_TRACE the register page is made to
 * fault and every access is answered from a copy of it -- so the title sees
 * exactly the plain memory it sees without the switch -- and printed:
 *
 *     [NIC] write +0x0A8 MacAddrA = 0x12F25000 (4) sub_0020FD1E+0x1B3
 *
 * A run of identical accesses (a poll loop) prints once with a count, and
 * every five seconds the timer thread prints which registers have been
 * touched and their last values. That is the list of registers a card model
 * has to give behaviour to, in the order XNet uses them.
 *
 *     RECOMP_NIC_TRACE=1             trap and log
 *     RECOMP_NIC_TRACE_BUDGET=<n>    access lines before only new registers
 *                                    print (default 2000)
 *
 * The register names are Linux forcedeth's for the same nForce MAC, there as
 * a reading aid. Offsets nobody named print bare.
 *
 * The card. Under RECOMP_SYSLINK=udp the registers have the behaviour XNet
 * relies on -- the idle bit after a reset, write-1-to-clear status, a kick
 * that sends -- and the card moves frames between the guest's descriptor
 * rings and the tunnel in xbox_net_udp.c, raising its interrupt on bus level
 * 4 from the kernel's timer thread. Sending happens on the kick itself;
 * received frames go in on the next timer tick.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "xbox_nic.h"
#include "xbox_net_udp.h"

#ifdef _WIN32
#include <bcrypt.h>
#include <dbghelp.h>
#include "platform/mmio_decode.h"
#endif

/* ---- the Ethernet address -------------------------------------------- */

static uint8_t g_mac[6];
static int     g_mac_ready;

static int hex_digit(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "00:50:F2:12:34:56", with ':' or '-' or nothing between the pairs. */
static int parse_mac(const char *s, uint8_t out[6])
{
    int i;

    for (i = 0; i < 6; i++) {
        int hi, lo;
        while (*s == ' ' || *s == '\t')
            s++;
        hi = hex_digit((unsigned char)s[0]);
        lo = hi < 0 ? -1 : hex_digit((unsigned char)s[1]);
        if (lo < 0)
            return 0;
        out[i] = (uint8_t)(hi << 4 | lo);
        s += 2;
        if (i < 5 && (*s == ':' || *s == '-'))
            s++;
    }
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    return *s == 0;
}

static void format_mac(const uint8_t m[6], char buf[18])
{
    snprintf(buf, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}

static int random_bytes(uint8_t *p, size_t n)
{
#ifdef _WIN32
    return BCryptGenRandom(NULL, p, (ULONG)n,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    size_t got = 0;
    if (f) {
        got = fread(p, 1, n, f);
        fclose(f);
    }
    return got == n;
#endif
}

static int identity_path(char *out, size_t n)
{
    char dir[1024];

    if (!xbox_path_save_dir_utf8(dir, sizeof dir))
        return 0;
#ifdef _WIN32
    snprintf(out, n, "%s\\console.ini", dir);
#else
    snprintf(out, n, "%s/console.ini", dir);
#endif
    return 1;
}

static int read_identity(const char *path, uint8_t mac[6])
{
    char line[256];
    FILE *f = fopen(path, "r");
    int found = 0;

    if (!f)
        return 0;
    while (!found && fgets(line, sizeof line, f)) {
        char *p = line, *eq;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == ';' || *p == '#' || strncmp(p, "mac", 3) != 0)
            continue;
        eq = strchr(p, '=');
        if (eq && parse_mac(eq + 1, mac))
            found = 1;
    }
    fclose(f);
    return found;
}

static void write_identity(const char *path, const uint8_t mac[6])
{
    char text[18];
    FILE *f = fopen(path, "w");

    if (!f) {
        fprintf(stderr, "[NET] cannot write %s; this address lasts one run\n",
                path);
        return;
    }
    format_mac(mac, text);
    fprintf(f,
        "; This console's identity on a network, written once on first run.\n"
        ";\n"
        "; mac is the Ethernet address System Link and XLink Kai see. Two\n"
        "; consoles in one game must not share one, so copying this folder to\n"
        "; a second PC that plays against this one needs a new address there:\n"
        "; delete this line and one is chosen. RECOMP_SYSLINK_MAC overrides it.\n"
        "mac = %s\n", text);
    fclose(f);
}

void xbox_NicMacAddress(uint8_t out[6])
{
    char text[18];

    if (!g_mac_ready) {
        const char *env = getenv("RECOMP_SYSLINK_MAC");
        const char *source = NULL;
        char path[1100];
        int have_path = identity_path(path, sizeof path);

        if (env && *env) {
            if (parse_mac(env, g_mac) && !(g_mac[0] & 1))
                source = "RECOMP_SYSLINK_MAC";
            else
                fprintf(stderr, "[NET] RECOMP_SYSLINK_MAC=%s is not a unicast "
                                "address (00:50:F2:12:34:56); ignored\n", env);
        }
        if (!source && have_path && read_identity(path, g_mac)) {
            if (!(g_mac[0] & 1))
                source = path;
            else
                fprintf(stderr, "[NET] %s holds a multicast address; "
                                "choosing a new one\n", path);
        }
        if (!source) {
            /* Microsoft's prefix, so a title that checks it is satisfied, and
             * 24 random bits under it: two players picking the same address
             * is a one-in-sixteen-million event per pair. */
            g_mac[0] = 0x00; g_mac[1] = 0x50; g_mac[2] = 0xF2;
            if (!random_bytes(g_mac + 3, 3)) {
                uint32_t t = (uint32_t)time(NULL) ^ (uint32_t)(uintptr_t)&t;
                g_mac[3] = (uint8_t)(t >> 16);
                g_mac[4] = (uint8_t)(t >> 8);
                g_mac[5] = (uint8_t)t;
            }
            if (have_path) {
                write_identity(path, g_mac);
                source = "new, saved to console.ini";
            } else {
                /* Asked before the save folder is known: nothing to keep it
                 * in, so this run has an address of its own and the next run
                 * chooses again. */
                source = "new, not saved (no save folder yet)";
            }
        }
        g_mac_ready = 1;
        format_mac(g_mac, text);
        fprintf(stderr, "[NET] Ethernet address %s (%s)\n", text, source);
        fflush(stderr);
    }
    memcpy(out, g_mac, 6);
}

/* ---- the registers ---------------------------------------------------- */

#ifdef _WIN32

#define NIC_DWORDS (XBOX_NIC_SIZE / 4)

/* Registers the model gives behaviour to. Offsets and bits are forcedeth's
 * names for the same nForce MAC; every one used here was seen in XNet's own
 * traffic (docs/technical/system-link.md). */
#define R_IRQ_STATUS   0x000u   /* write 1 to clear */
#define R_IRQ_MASK     0x004u
#define R_RX_CONTROL   0x094u   /* bit 0: receiver started */
#define R_TX_RING      0x100u
#define R_RX_RING      0x104u
#define R_RING_SIZES   0x108u   /* (receive - 1) << 16 | (send - 1) */
#define R_TXRX_CONTROL 0x144u
#define R_MII_STATUS   0x180u   /* write 1 to clear */
#define R_MII_CONTROL  0x190u
#define R_MII_DATA     0x194u

#define TXRX_KICK  0x01u        /* send what is queued */
#define TXRX_BIT2  0x04u        /* reset, as XNet uses it */
#define TXRX_IDLE  0x08u        /* XNet waits for this after a reset */
#define TXRX_RESET 0x10u

#define IRQ_RX       0x02u
#define IRQ_RX_NOBUF 0x04u
#define IRQ_TX_OK    0x10u

/* Descriptor flags (the high half of the second word). */
#define DESC_OWNED_BY_CARD 0x8000u  /* send: queued; receive: buffer free */
#define TX_LAST_FRAGMENT   0x0001u
#define TX_ERROR_BITS      0x7FF8u
#define RX_VALID           0x0001u  /* XNet skips a descriptor without it */
#define RX_BIT4            0x0010u  /* what xemu sets beside it */

#define MII_IN_USE 0x8000u

static struct {
    int      trapped;
    int      trace;                     /* RECOMP_NIC_TRACE: log accesses  */
    int      model;                     /* RECOMP_SYSLINK: give behaviour  */
    int      tunnel;                    /* 1 up, -1 failed, 0 not tried    */
    uint8_t *host;                      /* the page, for the fallback      */
    uint8_t  reg[XBOX_NIC_SIZE];        /* what the guest wrote, answered  */
    uint32_t reads[NIC_DWORDS], writes[NIC_DWORDS];
    unsigned long n_reads, n_writes, n_undecoded;
    long     budget;
    int      dirty;
    ULONGLONG last_summary;
    uintptr_t rip;                      /* of the access being serviced    */
    /* The previous line, so a poll loop prints once with a count. */
    struct { int w; uint32_t off, val; int size; uintptr_t rip; } prev;
    unsigned long repeats;

    /* The card. */
    uint32_t tx_index, rx_index;        /* next descriptor in each ring    */
    uint8_t  frame[XBOX_NET_FRAME_MAX]; /* a send being gathered           */
    uint32_t frame_len;
    int      frame_bad;
    int      rx_stalled;                /* reported "no buffer" already    */
    unsigned long tx_frames, tx_bad, rx_frames, rx_too_big, rx_waits;
    unsigned long buf_low, buf_window;  /* where frame buffers resolved    */
    CRITICAL_SECTION lock;
} s_nic;

static const char *reg_name(uint32_t off)
{
    switch (off & ~3u) {
    case 0x000: return "IrqStatus";
    case 0x004: return "IrqMask";
    case 0x008: return "UnknownSetupReg6";
    case 0x00C: return "PollingInterval";
    case 0x080: return "Misc1";
    case 0x084: return "TransmitterControl";
    case 0x088: return "TransmitterStatus";
    case 0x08C: return "PacketFilterFlags";
    case 0x090: return "OffloadConfig";
    case 0x094: return "ReceiverControl";
    case 0x098: return "ReceiverStatus";
    case 0x09C: return "RandomSeed";
    case 0x0A8: return "MacAddrA";
    case 0x0AC: return "MacAddrB";
    case 0x0B0: return "MulticastAddrA";
    case 0x0B4: return "MulticastAddrB";
    case 0x0B8: return "MulticastMaskA";
    case 0x0BC: return "MulticastMaskB";
    case 0x100: return "TxRingPhysAddr";
    case 0x104: return "RxRingPhysAddr";
    case 0x108: return "RingSizes";
    case 0x10C: return "TransmitPoll";
    case 0x110: return "LinkSpeed";
    case 0x13C: return "TxWatermark";
    case 0x144: return "TxRxControl";
    case 0x180: return "MIIStatus";
    case 0x184: return "MIIMask";
    case 0x188: return "AdapterControl";
    case 0x18C: return "MIISpeed";
    case 0x190: return "MIIControl";
    case 0x194: return "MIIData";
    default:    return "";
    }
}

static uint32_t reg32(uint32_t off)
{
    uint32_t v;
    memcpy(&v, s_nic.reg + off, 4);
    return v;
}

static void set_reg32(uint32_t off, uint32_t v)
{
    memcpy(s_nic.reg + off, &v, 4);
}

/* ---- guest memory, by physical address --------------------------------- */

/* A host pointer to len bytes at a physical address, through whichever
 * storage that page was handed out from (xbox_PhysToGuest). */
static uint8_t *phys_ptr(uint32_t phys, uint32_t len, int *low)
{
    uint8_t *base = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t va;

    if (!base || phys >= XBOX_CONTIG_SIZE || len > XBOX_CONTIG_SIZE - phys)
        return NULL;
    va = xbox_PhysToGuest(phys);
    if (low)
        *low = (va == phys);
    return base + va;
}

/* ---- the log ------------------------------------------------------------ */

static void print_rip(uintptr_t rip)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (SymFromAddr(GetCurrentProcess(), (DWORD64)rip, &disp, sym))
        fprintf(stderr, "%s+0x%llX", sym->Name, (unsigned long long)disp);
    else
        fprintf(stderr, "RIP 0x%llX", (unsigned long long)rip);
}

static void flush_repeats(void)
{
    if (s_nic.repeats) {
        fprintf(stderr, "[NIC]   ... and %lu more times\n", s_nic.repeats);
        s_nic.repeats = 0;
    }
}

static void log_access(int w, uint32_t off, uint64_t val, int size)
{
    uint32_t d = off / 4;
    int first = !(s_nic.reads[d] || s_nic.writes[d]);

    if (w) { s_nic.writes[d]++; s_nic.n_writes++; }
    else   { s_nic.reads[d]++;  s_nic.n_reads++;  }
    s_nic.dirty = 1;
    if (!s_nic.trace)
        return;

    if (!first && s_nic.prev.w == w && s_nic.prev.off == off &&
        s_nic.prev.val == (uint32_t)val && s_nic.prev.size == size &&
        s_nic.prev.rip == s_nic.rip) {
        s_nic.repeats++;
        return;
    }
    /* Past the budget, only a register nobody has touched yet still prints:
     * a long session's steady traffic stops, the first use of anything new
     * does not. */
    if (s_nic.budget <= 0 && !first)
        return;
    s_nic.budget--;

    flush_repeats();
    s_nic.prev.w = w; s_nic.prev.off = off; s_nic.prev.val = (uint32_t)val;
    s_nic.prev.size = size; s_nic.prev.rip = s_nic.rip;

    fprintf(stderr, "[NIC] %s +0x%03X %-18s %s 0x%0*llX (%d) ",
            w ? "write" : "read ", off, reg_name(off), w ? "=" : "->",
            size * 2, (unsigned long long)val, size);
    print_rip(s_nic.rip);
    fprintf(stderr, "%s\n", first ? "  [first]" : "");
}

/* ---- the card ------------------------------------------------------------ */

/* RECOMP_SYSLINK_FRAMES=<n>: one line for each of the first n frames the
 * card carries, either way. Enough of each header to follow a handshake:
 * the Ethernet addresses and type, and for IPv4 the addresses, protocol,
 * ports and length. The payload is XNet's and is encrypted past the first
 * exchange anyway. */
static void frame_line(const char *dir, const uint8_t *f, uint32_t len)
{
    static long budget = -2;
    if (budget == -2) {
        const char *e = getenv("RECOMP_SYSLINK_FRAMES");
        budget = e && *e ? strtol(e, NULL, 0) : 0;
    }
    if (budget <= 0 || len < 14)
        return;
    budget--;
    fprintf(stderr, "[NIC] %8llu ms %s %4u  %02X:%02X:%02X:%02X:%02X:%02X <- "
                    "%02X:%02X:%02X:%02X:%02X:%02X type %02X%02X",
            (unsigned long long)GetTickCount64(), dir, len,
            f[0], f[1], f[2], f[3], f[4], f[5],
            f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13]);
    if (f[12] == 0x08 && f[13] == 0x00 && len >= 34) {
        const uint8_t *ip = f + 14;
        uint32_t ihl = (ip[0] & 15u) * 4u;
        fprintf(stderr, "  %u.%u.%u.%u -> %u.%u.%u.%u proto %u",
                ip[12], ip[13], ip[14], ip[15], ip[16], ip[17], ip[18], ip[19],
                ip[9]);
        if ((ip[9] == 17 || ip[9] == 6) && 14 + ihl + 4 <= len)
            fprintf(stderr, " port %u -> %u",
                    ip[ihl] << 8 | ip[ihl + 1], ip[ihl + 2] << 8 | ip[ihl + 3]);
    } else if (f[12] == 0x08 && f[13] == 0x06 && len >= 42) {
        const uint8_t *a = f + 14;
        fprintf(stderr, "  ARP %s %u.%u.%u.%u -> %u.%u.%u.%u",
                a[7] == 1 ? "who-has" : "is-at",
                a[14], a[15], a[16], a[17], a[24], a[25], a[26], a[27]);
    }
    fprintf(stderr, "\n");
}

static uint32_t ring_count(int receive)
{
    uint32_t sizes = reg32(R_RING_SIZES);
    uint32_t n = (receive ? (sizes >> 16) : (sizes & 0xFFFFu)) + 1u;
    return n > 1024u ? 1024u : n;
}

/* The tunnel starts on the first register access rather than at init: by
 * then the save folder is known, so the Ethernet address is the console's
 * own and not a stand-in. */
static void tunnel_start(void)
{
    uint8_t mac[6];

    if (s_nic.tunnel)
        return;
    xbox_NicMacAddress(mac);
    s_nic.tunnel = xbox_NetUdpStart(mac) ? 1 : -1;
}

/* A kick: send everything queued from where the last kick stopped.
 *
 * A descriptor is the buffer's physical address, the length minus one, and
 * flags. The card owns it while bit 15 is set; handing it back is clearing
 * that bit, which is all XNet's completion path looks at before reclaiming
 * the buffer. A frame may span descriptors, ending at one marked last. */
static void transmit(void)
{
    uint32_t ring = reg32(R_TX_RING), count = ring_count(0), i;
    int sent = 0;

    if (!ring)
        return;
    for (i = 0; i < count; i++) {
        uint8_t *desc = phys_ptr(ring + s_nic.tx_index * 8u, 8, NULL);
        uint32_t buf;
        uint16_t len, flags;
        uint8_t *src;
        int low = 0;

        if (!desc)
            break;
        memcpy(&buf, desc, 4);
        memcpy(&len, desc + 4, 2);
        memcpy(&flags, desc + 6, 2);
        if (!(flags & DESC_OWNED_BY_CARD))
            break;

        src = phys_ptr(buf, (uint32_t)len + 1u, &low);
        if (low) s_nic.buf_low++; else s_nic.buf_window++;
        if (!src || s_nic.frame_len + len + 1u > sizeof s_nic.frame) {
            s_nic.frame_bad = 1;
        } else {
            memcpy(s_nic.frame + s_nic.frame_len, src, (size_t)len + 1u);
            s_nic.frame_len += (uint32_t)len + 1u;
        }
        if (flags & TX_LAST_FRAGMENT) {
            if (!s_nic.frame_bad && s_nic.frame_len >= 14 && s_nic.tunnel > 0) {
                frame_line("send", s_nic.frame, s_nic.frame_len);
                xbox_NetUdpSend(s_nic.frame, s_nic.frame_len);
                s_nic.tx_frames++;
            } else {
                s_nic.tx_bad++;
            }
            s_nic.frame_len = 0;
            s_nic.frame_bad = 0;
        }

        flags = (uint16_t)(flags & ~(DESC_OWNED_BY_CARD | TX_ERROR_BITS));
        memcpy(desc + 6, &flags, 2);
        s_nic.tx_index = (s_nic.tx_index + 1u) % count;
        sent = 1;
    }
    if (sent)
        set_reg32(R_IRQ_STATUS, reg32(R_IRQ_STATUS) | IRQ_TX_OK);
}

/* Received frames into the receive ring, on the timer thread.
 *
 * The receive length is the frame's own length -- XNet reads it as that, and
 * subtracts one only for an error combination -- and the flags say valid
 * with bit 15 clear. XNet re-arms each descriptor itself once it has taken
 * the frame. A frame with no free buffer waits in the tunnel's queue. */
static void receive(void)
{
    uint32_t ring = reg32(R_RX_RING), count = ring_count(1), n;
    uint8_t frame[XBOX_NET_FRAME_MAX];
    uint32_t len;

    if (!ring || !(reg32(R_RX_CONTROL) & 1u) || s_nic.tunnel <= 0)
        return;
    for (n = 0; n < count && (len = xbox_NetUdpPeek(frame, sizeof frame)) != 0; n++) {
        uint8_t *desc = phys_ptr(ring + s_nic.rx_index * 8u, 8, NULL);
        uint32_t buf;
        uint16_t cap, flags;
        uint8_t *dst;

        if (!desc)
            return;
        memcpy(&buf, desc, 4);
        memcpy(&cap, desc + 4, 2);
        memcpy(&flags, desc + 6, 2);
        if (!(flags & DESC_OWNED_BY_CARD)) {
            /* Every buffer is full. Say so once, as the hardware does;
             * XNet's answer is to restart the receiver after draining. */
            s_nic.rx_waits++;
            if (!s_nic.rx_stalled) {
                s_nic.rx_stalled = 1;
                set_reg32(R_IRQ_STATUS, reg32(R_IRQ_STATUS) | IRQ_RX_NOBUF);
            }
            return;
        }
        s_nic.rx_stalled = 0;
        xbox_NetUdpPop();
        dst = len <= (uint32_t)cap + 1u ? phys_ptr(buf, len, NULL) : NULL;
        if (!dst) {
            s_nic.rx_too_big++;
            continue;
        }
        memcpy(dst, frame, len);
        frame_line("recv", frame, len);
        cap = (uint16_t)len;
        flags = RX_VALID | RX_BIT4;
        memcpy(desc + 4, &cap, 2);
        memcpy(desc + 6, &flags, 2);
        s_nic.rx_index = (s_nic.rx_index + 1u) % count;
        s_nic.rx_frames++;
        set_reg32(R_IRQ_STATUS, reg32(R_IRQ_STATUS) | IRQ_RX);
        s_nic.dirty = 1;
    }
}

/* A PHY behind the MII registers that is always linked at 100 Mbit full
 * duplex. TimeSplitters 2 never asks -- the kernel's PhyGetLinkState answers
 * for it -- but a title that reads the PHY itself should find a cable. */
static uint16_t phy_read(uint32_t reg)
{
    switch (reg) {
    case 0:  return 0x3100;     /* control: 100 Mbit, autonegotiate, full   */
    case 1:  return 0x782D;     /* status: link up, negotiation complete    */
    case 4:  return 0x01E1;     /* we advertise 10/100, half/full           */
    case 5:  return 0x41E1;     /* and so does the other end                */
    default: return 0;
    }
}

static uint32_t card_read(uint32_t dword_off)
{
    uint32_t v = reg32(dword_off);

    if (!s_nic.model)
        return v;
    switch (dword_off) {
    case R_TXRX_CONTROL: return (v & ~TXRX_KICK) | TXRX_IDLE;
    case R_MII_CONTROL:  return v & ~MII_IN_USE;
    default:             return v;
    }
}

/* A write of `val` to the whole dword at dword_off, after merging a narrow
 * write into the current value -- or, for a write-1-to-clear register, the
 * bits written. */
static void card_write(uint32_t dword_off, uint32_t val, uint32_t bits)
{
    if (!s_nic.model) {
        set_reg32(dword_off, val);
        return;
    }
    switch (dword_off) {
    case R_IRQ_STATUS:
    case R_MII_STATUS:
        set_reg32(dword_off, reg32(dword_off) & ~bits);
        break;
    case R_TX_RING:
        set_reg32(dword_off, val);
        s_nic.tx_index = 0;
        s_nic.frame_len = 0;
        break;
    case R_RX_RING:
        set_reg32(dword_off, val);
        s_nic.rx_index = 0;
        break;
    case R_TXRX_CONTROL:
        if (val & (TXRX_BIT2 | TXRX_RESET)) {
            s_nic.tx_index = s_nic.rx_index = 0;
            s_nic.frame_len = 0;
        }
        set_reg32(dword_off, val & ~TXRX_KICK);
        if (val & TXRX_KICK)
            transmit();
        break;
    case R_MII_CONTROL:
        set_reg32(dword_off, val & ~MII_IN_USE);
        if (!(val & 0x400u))            /* a read: answer it at once */
            set_reg32(R_MII_DATA, phy_read(val & 0x1Fu));
        break;
    default:
        set_reg32(dword_off, val);
        break;
    }
}

static uint64_t nic_read(void *dev, uint32_t off, int size)
{
    uint32_t d = off & ~3u, shift = (off & 3u) * 8u;
    uint64_t v = 0;
    (void)dev;
    if (off + (uint32_t)size > XBOX_NIC_SIZE)
        return 0;
    if (s_nic.model && !s_nic.tunnel)
        tunnel_start();
    if (size == 8) {
        v = (uint64_t)card_read(d) | ((uint64_t)card_read(d + 4) << 32);
    } else {
        v = card_read(d) >> shift;
        if (size < 4)
            v &= (1ull << (size * 8)) - 1u;
    }
    log_access(0, off, v, size);
    return v;
}

static void nic_write(void *dev, uint32_t off, uint64_t val, int size)
{
    uint32_t d = off & ~3u, shift = (off & 3u) * 8u;
    (void)dev;
    if (off + (uint32_t)size > XBOX_NIC_SIZE)
        return;
    if (s_nic.model && !s_nic.tunnel)
        tunnel_start();
    log_access(1, off, val, size);
    if (size == 8) {
        card_write(d, (uint32_t)val, (uint32_t)val);
        card_write(d + 4, (uint32_t)(val >> 32), (uint32_t)(val >> 32));
    } else if (size == 4) {
        card_write(d, (uint32_t)val, (uint32_t)val);
    } else {
        uint32_t mask = (uint32_t)((1ull << (size * 8)) - 1u) << shift;
        uint32_t bits = ((uint32_t)val << shift) & mask;
        card_write(d, (reg32(d) & ~mask) | bits, bits);
    }
}

/* Stop trapping and leave the page as the plain memory it was, holding what
 * the guest wrote. An instruction the decoder does not know must not stop
 * the title. With only the log on, the title would have run on plain memory
 * anyway; with the card on, System Link stops here and says so. */
static void untrap(const char *why)
{
    DWORD old;

    flush_repeats();
    if (VirtualProtect(s_nic.host, XBOX_NIC_SIZE, PAGE_READWRITE, &old))
        memcpy(s_nic.host, s_nic.reg, XBOX_NIC_SIZE);
    s_nic.trapped = 0;
    s_nic.model = 0;
    fprintf(stderr, "[NIC] register trap stopped: %s; the page is plain "
                    "memory again%s\n", why,
            s_nic.tunnel > 0 ? " and System Link is off" : "");
    fflush(stderr);
}

void xbox_NicInit(void *host)
{
    const char *trace = getenv("RECOMP_NIC_TRACE");
    const char *budget = getenv("RECOMP_NIC_TRACE_BUDGET");
    const char *link = getenv("RECOMP_SYSLINK");
    DWORD old;

    s_nic.trace = trace && *trace && strcmp(trace, "0") != 0;
    s_nic.model = link && *link && strcmp(link, "0") != 0 &&
                  _stricmp(link, "off") != 0;
    if (s_nic.model && _stricmp(link, "udp") != 0) {
        fprintf(stderr, "[NIC] RECOMP_SYSLINK=%s: only \"udp\" exists so far; "
                        "the card stays off\n", link);
        s_nic.model = 0;
    }
    if (!host || (!s_nic.trace && !s_nic.model))
        return;

    InitializeCriticalSection(&s_nic.lock);
    s_nic.budget = (budget && *budget) ? strtol(budget, NULL, 0) : 2000;
    memcpy(s_nic.reg, host, XBOX_NIC_SIZE);
    if (!VirtualProtect(host, XBOX_NIC_SIZE, PAGE_NOACCESS, &old)) {
        fprintf(stderr, "[NIC] cannot trap 0x%08X (error %lu); no card\n",
                XBOX_NIC_BASE, GetLastError());
        s_nic.model = 0;
        return;
    }
    s_nic.trapped = 1;
    s_nic.host = (uint8_t *)host;       /* last: says the trap is armed */
    s_nic.last_summary = GetTickCount64();
    fprintf(stderr, "[NIC] registers at 0x%08X trapped: %s%s\n", XBOX_NIC_BASE,
            s_nic.model ? "the card is on (RECOMP_SYSLINK)"
                        : "plain memory, logged",
            s_nic.model && s_nic.trace ? ", logged" : "");
    fflush(stderr);
}

int xbox_NicHandleMmio(void *ctx, uint32_t xbox_va, int is_write)
{
    PCONTEXT c = (PCONTEXT)ctx;
    int ok;

    (void)is_write;
    /* Whether the page was ever trapped, not whether it still is: a fault
     * already in flight on another thread when the trap stops must be
     * retried on the now-plain page below, not declined as a crash. */
    if (!s_nic.host || xbox_va < XBOX_NIC_BASE ||
        xbox_va >= XBOX_NIC_BASE + XBOX_NIC_SIZE)
        return 0;

    EnterCriticalSection(&s_nic.lock);
    if (!s_nic.trapped) {               /* untrapped by another thread */
        LeaveCriticalSection(&s_nic.lock);
        return 1;                       /* retry on plain memory */
    }
    s_nic.rip = (uintptr_t)c->Rip;
    ok = mmio_emulate(c, xbox_va - XBOX_NIC_BASE, &s_nic, nic_read, nic_write);
    if (!ok) {
        const uint8_t *ip = (const uint8_t *)c->Rip;
        char why[160];
        s_nic.n_undecoded++;
        snprintf(why, sizeof why, "undecoded access at +0x%03X, host bytes "
                 "%02X %02X %02X %02X %02X %02X", xbox_va - XBOX_NIC_BASE,
                 ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        untrap(why);
        /* Not stepped over: returning handled re-runs the instruction, now
         * on plain memory, which is what it would have done untraced. */
    }
    if (s_nic.trace)
        fflush(stderr);
    LeaveCriticalSection(&s_nic.lock);
    return 1;
}

int xbox_NicIrqPending(void)
{
    return s_nic.model &&
           (reg32(R_IRQ_STATUS) & reg32(R_IRQ_MASK)) != 0;
}

/* ---- the summary --------------------------------------------------------- */

static void print_frame_start(const uint8_t *f)
{
    int b;
    fprintf(stderr, "[NIC]          dst %02X:%02X:%02X:%02X:%02X:%02X"
                    " src %02X:%02X:%02X:%02X:%02X:%02X type %02X%02X:",
            f[0], f[1], f[2], f[3], f[4], f[5],
            f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13]);
    for (b = 14; b < 48; b++)
        fprintf(stderr, " %02X", f[b]);
    fprintf(stderr, "\n");
}

/* The descriptor rings, as the guest left them: the buffer address, the
 * length and flag words, and the start of each queued frame. */
static void print_ring(const char *which, uint32_t phys, uint32_t count,
                       int frames)
{
    uint32_t i;
    const uint8_t *ring = phys_ptr(phys, count * 8, NULL);

    fprintf(stderr, "[NIC]   %s ring at physical 0x%08X, %u descriptors\n",
            which, phys, count);
    if (!ring)
        return;
    for (i = 0; i < count; i++) {
        uint32_t buf;
        uint16_t len, flags;
        int low = 0;
        const uint8_t *f;
        memcpy(&buf, ring + i * 8, 4);
        memcpy(&len, ring + i * 8 + 4, 2);
        memcpy(&flags, ring + i * 8 + 6, 2);
        f = buf ? phys_ptr(buf, 48, &low) : NULL;
        fprintf(stderr, "[NIC]     [%2u] buffer 0x%08X (%s) length 0x%04X "
                        "flags 0x%04X\n", i, buf, low ? "low RAM" : "window",
                len, flags);
        if (frames && f)
            print_frame_start(f);
    }
}

void xbox_NicTick(void)
{
    ULONGLONG now;
    uint32_t d;

    if (!s_nic.trapped)
        return;
    if (s_nic.model) {
        EnterCriticalSection(&s_nic.lock);
        receive();
        LeaveCriticalSection(&s_nic.lock);
    }
    if (!s_nic.dirty)
        return;
    now = GetTickCount64();
    if (now - s_nic.last_summary < 5000)
        return;

    EnterCriticalSection(&s_nic.lock);
    s_nic.last_summary = now;
    s_nic.dirty = 0;
    flush_repeats();
    if (s_nic.model) {
        unsigned long sent = 0, got = 0, dropped = 0;
        xbox_NetUdpStats(&sent, &got, &dropped);
        fprintf(stderr, "[NIC] card: sent %lu frames (%lu refused), received "
                        "%lu (%lu too big, %lu waits for a buffer); tunnel "
                        "%lu out, %lu in, %lu dropped; send buffers in low RAM "
                        "%lu, window %lu\n",
                s_nic.tx_frames, s_nic.tx_bad, s_nic.rx_frames,
                s_nic.rx_too_big, s_nic.rx_waits, sent, got, dropped,
                s_nic.buf_low, s_nic.buf_window);
    }
    if (s_nic.trace) {
        fprintf(stderr, "[NIC] summary: %lu reads, %lu writes, %lu undecoded\n",
                s_nic.n_reads, s_nic.n_writes, s_nic.n_undecoded);
        for (d = 0; d < NIC_DWORDS; d++) {
            if (!s_nic.reads[d] && !s_nic.writes[d])
                continue;
            fprintf(stderr, "[NIC]   +0x%03X %-18s %6u reads %6u writes  "
                            "now 0x%08X\n", d * 4, reg_name(d * 4),
                    s_nic.reads[d], s_nic.writes[d], reg32(d * 4));
        }
        /* The rings, for the first few summaries after XNet sets them up:
         * enough to see the format and the first frames, not a stream. */
        {
            static int shown;
            if (reg32(R_TX_RING) && shown < 3) {
                shown++;
                print_ring("send", reg32(R_TX_RING), ring_count(0), 1);
                print_ring("receive", reg32(R_RX_RING), ring_count(1), 0);
            }
        }
    }
    fflush(stderr);
    LeaveCriticalSection(&s_nic.lock);
}

#else /* !_WIN32 */

/* No trap on POSIX yet: the page stays plain memory, as it was. */
void xbox_NicInit(void *host) { (void)host; }
int  xbox_NicHandleMmio(void *ctx, uint32_t xbox_va, int is_write)
{
    (void)ctx; (void)xbox_va; (void)is_write;
    return 0;
}
int  xbox_NicIrqPending(void) { return 0; }
void xbox_NicTick(void) { }

#endif
