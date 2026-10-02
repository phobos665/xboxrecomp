/**
 * The console's network card, as far as System Link needs it so far.
 *
 * System Link is plain Ethernet, and the network stack that speaks it (XNet)
 * is linked into every title and drives the card's registers itself. So the
 * card is the boundary: below it a frame is a byte array, above it is the
 * title's own lifted code. docs/technical/system-link.md has the plan.
 *
 * Two pieces live here today.
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "kernel.h"
#include "xbox_memory_layout.h"
#include "xbox_nic.h"

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

/* ---- the register log ------------------------------------------------- */

#ifdef _WIN32

#define NIC_DWORDS (XBOX_NIC_SIZE / 4)

static struct {
    int      trapped;
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

static uint64_t nic_read(void *dev, uint32_t off, int size)
{
    uint64_t v = 0;
    (void)dev;
    if (off + (uint32_t)size > XBOX_NIC_SIZE)
        return 0;
    memcpy(&v, s_nic.reg + off, (size_t)size);
    log_access(0, off, v, size);
    return v;
}

static void nic_write(void *dev, uint32_t off, uint64_t val, int size)
{
    (void)dev;
    if (off + (uint32_t)size > XBOX_NIC_SIZE)
        return;
    memcpy(s_nic.reg + off, &val, (size_t)size);
    log_access(1, off, val, size);
}

/* Stop trapping and leave the page as the plain memory it was, holding what
 * the guest wrote. An instruction the decoder does not know must not stop
 * the title -- without the trace it would have run on plain memory, so it
 * still does, and the log says where the record ends. */
static void untrap(const char *why)
{
    DWORD old;

    flush_repeats();
    if (VirtualProtect(s_nic.host, XBOX_NIC_SIZE, PAGE_READWRITE, &old))
        memcpy(s_nic.host, s_nic.reg, XBOX_NIC_SIZE);
    s_nic.trapped = 0;
    fprintf(stderr, "[NIC] register log stopped: %s; the page is plain "
                    "memory again\n", why);
    fflush(stderr);
}

void xbox_NicInit(void *host)
{
    const char *trace = getenv("RECOMP_NIC_TRACE");
    const char *budget = getenv("RECOMP_NIC_TRACE_BUDGET");
    DWORD old;

    if (!host || !trace || !*trace || strcmp(trace, "0") == 0)
        return;

    InitializeCriticalSection(&s_nic.lock);
    s_nic.budget = (budget && *budget) ? strtol(budget, NULL, 0) : 2000;
    memcpy(s_nic.reg, host, XBOX_NIC_SIZE);
    if (!VirtualProtect(host, XBOX_NIC_SIZE, PAGE_NOACCESS, &old)) {
        fprintf(stderr, "[NIC] cannot trap 0x%08X (error %lu); no register "
                        "log\n", XBOX_NIC_BASE, GetLastError());
        return;
    }
    s_nic.trapped = 1;
    s_nic.host = (uint8_t *)host;       /* last: says the trap is armed */
    s_nic.last_summary = GetTickCount64();
    fprintf(stderr, "[NIC] registers at 0x%08X trapped and logged "
                    "(RECOMP_NIC_TRACE); they still behave as plain memory\n",
            XBOX_NIC_BASE);
    fflush(stderr);
}

int xbox_NicHandleMmio(void *ctx, uint32_t xbox_va, int is_write)
{
    PCONTEXT c = (PCONTEXT)ctx;
    int ok;

    (void)is_write;
    /* Whether the page was ever trapped, not whether it still is: a fault
     * already in flight on another thread when the log stops must be
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
    fflush(stderr);
    LeaveCriticalSection(&s_nic.lock);
    return 1;
}

/* The descriptor rings, as the guest left them.
 *
 * XNet hands the card two rings of eight-byte descriptors in contiguous
 * memory -- the register log shows their physical addresses and sizes --
 * and queues frames there, not through registers. Printing them is how the
 * descriptor format gets checked before a model depends on it: the buffer
 * address, the length and flag words, and the start of each queued frame.
 *
 * Physical addresses are in the contiguous window, which this runtime maps
 * at 0x80000000 + physical. RingSizes packs (receive - 1) << 16 | (send - 1),
 * as forcedeth writes it. */
#define NIC_CONTIG_BASE 0x80000000u
#define NIC_CONTIG_SIZE (64u * 1024u * 1024u)

static const uint8_t *guest_phys(uint32_t phys, uint32_t len)
{
    uint8_t *base = (uint8_t *)xbox_GetMemoryOffset();
    if (!base || phys >= NIC_CONTIG_SIZE || len > NIC_CONTIG_SIZE - phys)
        return NULL;
    return base + NIC_CONTIG_BASE + phys;
}

/* The same physical address read as low RAM, which is identity mapped:
 * MmGetPhysicalAddress answers a low VA with itself, and low RAM is separate
 * storage from the window here (xbox_memory_layout.c), so a physical address
 * on its own does not say which of the two it came from. */
static const uint8_t *guest_low(uint32_t phys, uint32_t len)
{
    uint8_t *base = (uint8_t *)xbox_GetMemoryOffset();
    if (!base || phys >= NIC_CONTIG_SIZE || len > NIC_CONTIG_SIZE - phys)
        return NULL;
    return base + phys;
}

static void print_frame_start(const char *view, const uint8_t *f)
{
    int b;
    fprintf(stderr, "[NIC]          %-6s dst %02X:%02X:%02X:%02X:%02X:%02X"
                    " src %02X:%02X:%02X:%02X:%02X:%02X type %02X%02X:",
            view, f[0], f[1], f[2], f[3], f[4], f[5],
            f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[13]);
    for (b = 14; b < 48; b++)
        fprintf(stderr, " %02X", f[b]);
    fprintf(stderr, "\n");
}

static uint32_t reg32(uint32_t off)
{
    uint32_t v;
    memcpy(&v, s_nic.reg + off, 4);
    return v;
}

static void print_ring(const char *which, uint32_t phys, uint32_t count,
                       int frames)
{
    uint32_t i;
    const uint8_t *ring = guest_phys(phys, count * 8);

    fprintf(stderr, "[NIC]   %s ring at physical 0x%08X, %u descriptors\n",
            which, phys, count);
    if (!ring)
        return;
    for (i = 0; i < count; i++) {
        uint32_t buf;
        uint16_t len, flags;
        memcpy(&buf, ring + i * 8, 4);
        memcpy(&len, ring + i * 8 + 4, 2);
        memcpy(&flags, ring + i * 8 + 6, 2);
        fprintf(stderr, "[NIC]     [%2u] buffer 0x%08X length 0x%04X "
                        "flags 0x%04X\n", i, buf, len, flags);
        if (frames && buf) {
            /* The Ethernet header and what follows it: destination,
             * source, type, then the first bytes of the payload -- through
             * both storages, because only one of them holds the frame. */
            const uint8_t *w = guest_phys(buf, 48), *l = guest_low(buf, 48);
            if (w)
                print_frame_start("window", w);
            if (l)
                print_frame_start("low", l);
        }
    }
}

void xbox_NicTick(void)
{
    ULONGLONG now;
    uint32_t d;

    if (!s_nic.trapped || !s_nic.dirty)
        return;
    now = GetTickCount64();
    if (now - s_nic.last_summary < 5000)
        return;

    EnterCriticalSection(&s_nic.lock);
    s_nic.last_summary = now;
    s_nic.dirty = 0;
    flush_repeats();
    fprintf(stderr, "[NIC] summary: %lu reads, %lu writes, %lu undecoded\n",
            s_nic.n_reads, s_nic.n_writes, s_nic.n_undecoded);
    for (d = 0; d < NIC_DWORDS; d++) {
        uint32_t v;
        if (!s_nic.reads[d] && !s_nic.writes[d])
            continue;
        memcpy(&v, s_nic.reg + d * 4, 4);
        fprintf(stderr, "[NIC]   +0x%03X %-18s %6u reads %6u writes  "
                        "now 0x%08X\n", d * 4, reg_name(d * 4),
                s_nic.reads[d], s_nic.writes[d], v);
    }
    /* The rings, for the first few summaries after XNet sets them up:
     * enough to see the format and the first frames, not a stream. */
    {
        static int shown;
        uint32_t sizes = reg32(0x108);
        if (reg32(0x100) && shown < 3) {
            shown++;
            print_ring("send", reg32(0x100), (sizes & 0xFFFFu) + 1, 1);
            print_ring("receive", reg32(0x104), (sizes >> 16) + 1, 0);
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
void xbox_NicTick(void) { }

#endif
