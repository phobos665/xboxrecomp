/**
 * The network card's cable: Ethernet frames over UDP.
 *
 * Each frame the card sends goes out as one UDP datagram holding the bare
 * frame, nothing added, the moment the title queues it. Each datagram that
 * arrives is a frame for the card. That is the format of xemu's UDP tunnel
 * (QEMU's datagram socket backend), so an xemu on the other end, or XLink
 * Kai's xemu port at 127.0.0.1:34523, needs nothing from us but an address.
 *
 *     RECOMP_SYSLINK=udp                      the card is on, through this
 *     RECOMP_SYSLINK_REMOTE=127.0.0.1:9002    where frames go
 *     RECOMP_SYSLINK_LOCAL=9001               the port to receive on
 *                                             (default: any free port)
 *
 * Two copies on one PC are two tunnels pointed at each other's ports.
 *
 * And for trying a title at internet distances on one PC, applied to
 * frames as they arrive:
 *
 *     RECOMP_SYSLINK_DELAY_MS=80    hold every frame back this long
 *     RECOMP_SYSLINK_LOSS=2         drop this percentage of them
 *
 * Received frames wait in a queue until the card has a receive buffer for
 * them; the card takes them on the kernel's timer thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "xbox_net_udp.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET net_socket;
#define NET_BAD_SOCKET INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
typedef int net_socket;
#define NET_BAD_SOCKET (-1)
#endif

#define QUEUE_LEN 256

static struct {
    net_socket sock;
    struct sockaddr_storage remote;
    int        remote_len;
    uint8_t    own_mac[6];
    long       delay_ms;
    int        loss_pct;
    uint32_t   rng;

    /* Received frames, oldest first. */
    struct { uint32_t len; unsigned long long due; uint8_t data[XBOX_NET_FRAME_MAX]; }
               q[QUEUE_LEN];
    unsigned   head, count;
    unsigned long sent, received, dropped;
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} s_udp = { NET_BAD_SOCKET };

static void lock(void)
{
#ifdef _WIN32
    EnterCriticalSection(&s_udp.lock);
#else
    pthread_mutex_lock(&s_udp.lock);
#endif
}

static void unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_udp.lock);
#else
    pthread_mutex_unlock(&s_udp.lock);
#endif
}

static unsigned long long now_ms(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000u + (unsigned long long)t.tv_nsec / 1000000u;
#endif
}

/* xorshift32: loss injection wants something cheap, not something good. */
static uint32_t next_random(void)
{
    uint32_t x = s_udp.rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return s_udp.rng = x;
}

static void receive_one(const uint8_t *buf, int n)
{
    /* Not a frame, or one of ours coming back (a relay that echoes, or a
     * misconfigured pair pointed at itself). */
    if (n < 14 || n > (int)XBOX_NET_FRAME_MAX || memcmp(buf + 6, s_udp.own_mac, 6) == 0)
        return;
    lock();
    if (s_udp.loss_pct > 0 && (int)(next_random() % 100u) < s_udp.loss_pct) {
        s_udp.dropped++;
    } else if (s_udp.count == QUEUE_LEN) {
        s_udp.dropped++;
    } else {
        unsigned i = (s_udp.head + s_udp.count) % QUEUE_LEN;
        s_udp.q[i].len = (uint32_t)n;
        s_udp.q[i].due = now_ms() + (unsigned long long)s_udp.delay_ms;
        memcpy(s_udp.q[i].data, buf, (size_t)n);
        s_udp.count++;
        s_udp.received++;
    }
    unlock();
}

#ifdef _WIN32
static DWORD WINAPI receive_thread(LPVOID unused)
#else
static void *receive_thread(void *unused)
#endif
{
    uint8_t buf[2048];

    (void)unused;
    for (;;) {
        int n = (int)recvfrom(s_udp.sock, (char *)buf, (int)sizeof buf, 0, NULL, NULL);
        if (n < 0) {
#ifdef _WIN32
            /* A datagram to a port nobody is listening on yet comes back as
             * an ICMP error, which Windows reports on the next receive as
             * WSAECONNRESET. It says nothing about this socket. */
            if (WSAGetLastError() == WSAECONNRESET)
                continue;
#endif
            fprintf(stderr, "[NET] receive failed; the tunnel stops receiving\n");
            fflush(stderr);
            break;
        }
        receive_one(buf, n);
    }
    return 0;
}

/* "host:port" to an address. */
static int resolve(const char *spec, struct sockaddr_storage *out, int *out_len)
{
    char host[256];
    const char *colon = strrchr(spec, ':');
    struct addrinfo hints, *res = NULL;
    size_t hl;

    if (!colon || colon == spec)
        return 0;
    hl = (size_t)(colon - spec);
    if (hl >= sizeof host)
        return 0;
    memcpy(host, spec, hl);
    host[hl] = 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, colon + 1, &hints, &res) != 0 || !res)
        return 0;
    memcpy(out, res->ai_addr, res->ai_addrlen);
    *out_len = (int)res->ai_addrlen;
    freeaddrinfo(res);
    return 1;
}

int xbox_NetUdpStart(const uint8_t own_mac[6])
{
    const char *remote = getenv("RECOMP_SYSLINK_REMOTE");
    const char *local = getenv("RECOMP_SYSLINK_LOCAL");
    const char *delay = getenv("RECOMP_SYSLINK_DELAY_MS");
    const char *loss = getenv("RECOMP_SYSLINK_LOSS");
    struct sockaddr_in bind_addr;
    int port = local && *local ? atoi(local) : 0;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[NET] Winsock did not start; no System Link\n");
        return 0;
    }
    InitializeCriticalSection(&s_udp.lock);
#else
    pthread_mutex_init(&s_udp.lock, NULL);
#endif

    if (!remote || !*remote || !resolve(remote, &s_udp.remote, &s_udp.remote_len)) {
        fprintf(stderr, "[NET] RECOMP_SYSLINK_REMOTE=%s is not host:port; "
                        "no System Link\n", remote ? remote : "(unset)");
        return 0;
    }

    s_udp.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp.sock == NET_BAD_SOCKET) {
        fprintf(stderr, "[NET] no UDP socket; no System Link\n");
        return 0;
    }
    memset(&bind_addr, 0, sizeof bind_addr);
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons((unsigned short)port);
    if (bind(s_udp.sock, (struct sockaddr *)&bind_addr, sizeof bind_addr) != 0) {
        fprintf(stderr, "[NET] cannot receive on UDP port %d (in use?); "
                        "no System Link\n", port);
        return 0;
    }
    if (!port) {
        socklen_t bl = sizeof bind_addr;
        getsockname(s_udp.sock, (struct sockaddr *)&bind_addr, &bl);
        port = ntohs(bind_addr.sin_port);
    }

    memcpy(s_udp.own_mac, own_mac, 6);
    s_udp.delay_ms = delay && *delay ? strtol(delay, NULL, 10) : 0;
    s_udp.loss_pct = loss && *loss ? atoi(loss) : 0;
    s_udp.rng = 0x9E3779B9u ^ (uint32_t)now_ms();

#ifdef _WIN32
    {
        HANDLE t = CreateThread(NULL, 0, receive_thread, NULL, 0, NULL);
        if (!t) {
            fprintf(stderr, "[NET] no receive thread; no System Link\n");
            return 0;
        }
        CloseHandle(t);
    }
#else
    {
        pthread_t t;
        if (pthread_create(&t, NULL, receive_thread, NULL) != 0) {
            fprintf(stderr, "[NET] no receive thread; no System Link\n");
            return 0;
        }
        pthread_detach(t);
    }
#endif

    fprintf(stderr, "[NET] System Link over UDP: receiving on port %d, sending "
                    "to %s", port, remote);
    if (s_udp.delay_ms || s_udp.loss_pct)
        fprintf(stderr, " (adding %ld ms and %d%% loss to what arrives)",
                s_udp.delay_ms, s_udp.loss_pct);
    fprintf(stderr, "\n");
    fflush(stderr);
    return 1;
}

void xbox_NetUdpSend(const uint8_t *frame, uint32_t len)
{
    if (s_udp.sock == NET_BAD_SOCKET || len > XBOX_NET_FRAME_MAX)
        return;
    if (sendto(s_udp.sock, (const char *)frame, (int)len, 0,
               (const struct sockaddr *)&s_udp.remote, s_udp.remote_len) == (int)len)
        s_udp.sent++;
}

uint32_t xbox_NetUdpPeek(uint8_t *out, uint32_t cap)
{
    uint32_t len = 0;

    if (s_udp.sock == NET_BAD_SOCKET)
        return 0;
    lock();
    if (s_udp.count && s_udp.q[s_udp.head].due <= now_ms()) {
        len = s_udp.q[s_udp.head].len;
        if (len > cap)
            len = cap;
        memcpy(out, s_udp.q[s_udp.head].data, len);
    }
    unlock();
    return len;
}

void xbox_NetUdpPop(void)
{
    if (s_udp.sock == NET_BAD_SOCKET)
        return;
    lock();
    if (s_udp.count) {
        s_udp.head = (s_udp.head + 1) % QUEUE_LEN;
        s_udp.count--;
    }
    unlock();
}

void xbox_NetUdpStats(unsigned long *sent, unsigned long *received,
                      unsigned long *dropped)
{
    if (sent) *sent = s_udp.sent;
    if (received) *received = s_udp.received;
    if (dropped) *dropped = s_udp.dropped;
}
