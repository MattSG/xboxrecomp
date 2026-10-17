/*
 * net_platform.h -- the socket calls where Winsock and BSD sockets differ.
 *
 * net_backend.c is one implementation; only these shims know the host. The
 * threads, events, SRW locks and timeouts it uses come from the platform
 * layer (win32_compat.h off Windows), which is the same on both.
 */
#ifndef NET_PLATFORM_H
#define NET_PLATFORM_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

typedef int net_socklen_t;

static inline int net_sock_startup(void)
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

static inline void net_sock_close(SOCKET s) { closesocket(s); }

static inline void net_sock_set_nonblocking(SOCKET s, int on)
{
    u_long v = on ? 1 : 0;
    ioctlsocket(s, FIONBIO, &v);
}

static inline void net_sock_set_broadcast(SOCKET s)
{
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);
}

static inline void net_sock_set_recv_timeout(SOCKET s, unsigned ms)
{
    DWORD tmo = ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
}

/* An ICMP port-unreachable must not turn into an error on the next read. */
static inline void net_sock_ignore_udp_reset(SOCKET s)
{
    BOOL off = FALSE;
    DWORD ret;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off,
             NULL, 0, &ret, NULL, NULL);
}

/* The host's first DNS server (network order), 0 if none. */
static inline uint32_t net_host_dns_first(void)
{
    ULONG size = 0;
    GetNetworkParams(NULL, &size);
    FIXED_INFO *info = (FIXED_INFO *)malloc(size);
    struct in_addr a;
    uint32_t r = 0;
    if (info && GetNetworkParams(info, &size) == NO_ERROR &&
        inet_pton(AF_INET, info->DnsServerList.IpAddress.String, &a) == 1)
        r = a.s_addr;
    free(info);
    return r;
}

#else /* POSIX: Linux, macOS */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "../platform/win32_compat.h"   /* Sleep, events, SRW locks, threads */

typedef int SOCKET;
#define INVALID_SOCKET (-1)
typedef socklen_t net_socklen_t;

static inline int net_sock_startup(void) { return 1; }

static inline void net_sock_close(SOCKET s) { close(s); }

static inline void net_sock_set_nonblocking(SOCKET s, int on)
{
    int fl = fcntl(s, F_GETFL, 0);
    if (fl >= 0)
        fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

static inline void net_sock_set_broadcast(SOCKET s)
{
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof yes);
}

static inline void net_sock_set_recv_timeout(SOCKET s, unsigned ms)
{
    struct timeval tv = {(time_t)(ms / 1000), (suseconds_t)(ms % 1000) * 1000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* Unconnected UDP sockets do not surface ICMP errors here, so nothing to do. */
static inline void net_sock_ignore_udp_reset(SOCKET s) { (void)s; }

/* The first nameserver in /etc/resolv.conf (network order), 0 if none. */
static inline uint32_t net_host_dns_first(void)
{
    char line[256];
    struct in_addr a;
    uint32_t r = 0;
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f)
        return 0;
    while (!r && fgets(line, sizeof line, f)) {
        char ip[64];
        if (sscanf(line, " nameserver %63s", ip) == 1 && inet_pton(AF_INET, ip, &a) == 1)
            r = a.s_addr;
    }
    fclose(f);
    return r;
}
#endif

/* Read-set bookkeeping: select() wants the highest descriptor + 1 on POSIX
 * (Winsock ignores it), and the count says whether there is anything to wait
 * on at all. */
typedef struct { int count; int nfds; } NetWatch;

static inline void net_watch(fd_set *rd, NetWatch *w, SOCKET s)
{
    FD_SET(s, rd);
    w->count++;
    if ((int)s >= w->nfds)
        w->nfds = (int)s + 1;
}

#endif /* NET_PLATFORM_H */
