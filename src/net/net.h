/*
 * net.h -- the Xbox network adapter (nvnet.c) and what carries its frames
 * (net_backend.c).
 *
 * RECOMP_NET=nat turns it on. The adapter is the MCPX Ethernet controller a
 * title's XNet drives; the backend is a user-mode NAT through the host's
 * sockets (DHCP, DNS, UDP), plus, with RECOMP_NET_PEERS, a tunnel that
 * carries Ethernet frames to other players over UDP so system link works
 * across the internet as it does on one LAN.
 */
#ifndef XBOX_NET_H
#define XBOX_NET_H

#include <stdint.h>

/* The adapter: does nothing unless RECOMP_NET is set. */
void xbox_NicInit(void);
int  xbox_NicOwnsAddress(uint32_t xbox_va);
int  xbox_NicHandleMmio(void *ctx, uint32_t xbox_va);  /* ctx: PCONTEXT */
void xbox_NicReport(void);

/* Optional DNS aliases, configured before network startup. The caller owns
 * the table and non-NULL strings for the backend's lifetime; NULL clears it.
 * No aliases are installed by default. A/IN requests for matching names resolve
 * through the host; other names retain ordinary upstream DNS forwarding. */
typedef struct { const char *query, *target; } XboxDnsAlias;
void net_backend_set_dns_aliases(const XboxDnsAlias *aliases, unsigned count);

/* The backend, called by the adapter. */
int  net_backend_init(const char *mode);
void net_backend_send(const uint8_t *frame, uint32_t len);   /* guest -> network */
uint32_t net_backend_peek(uint8_t *buf, uint32_t cap);      /* next frame for the guest, 0 none */
void net_backend_pop(void);
void net_backend_wait(unsigned ms);                          /* until a frame is queued or ms pass */

#endif
