/* The network backend from the guest's side: DHCP gives an address, ARP finds
 * the router, UDP goes out through a host socket and its reply comes back as
 * a frame, and frames cross the peer tunnel both ways. */
#include "../../src/net/net_backend.c"
#include <stdio.h>

static const uint8_t guest[6] = {0x00, 0x50, 0xF2, 0x01, 0x02, 0x03};

/* The next frame for the guest within ms, or 0. */
static uint32_t next_frame(uint8_t *buf, unsigned ms)
{
    for (unsigned t = 0; t < ms; t += 5) {
        uint32_t n = net_backend_peek(buf, FRAME_MAX);
        if (n) {
            net_backend_pop();
            return n;
        }
        Sleep(5);
    }
    return 0;
}

/* A guest UDP frame to the router's MAC (or broadcast). */
static uint32_t udp_frame(uint8_t *f, const uint8_t *dmac, uint32_t sip, uint32_t dip,
                          uint16_t sport, uint16_t dport, const void *data, uint32_t n)
{
    memcpy(f, dmac, 6);
    memcpy(f + 6, guest, 6);
    wr16(f + 12, 0x0800);
    uint8_t *ip = f + 14, *udp = ip + 20;
    memset(ip, 0, 20);
    ip[0] = 0x45; wr16(ip + 2, (uint16_t)(28 + n)); ip[8] = 64; ip[9] = 17;
    wr32(ip + 12, sip); wr32(ip + 16, dip);
    wr16(udp, sport); wr16(udp + 2, dport); wr16(udp + 4, (uint16_t)(8 + n)); wr16(udp + 6, 0);
    memcpy(udp + 8, data, n);
    return 14 + 28 + n;
}

int main(void)
{
    uint8_t f[FRAME_MAX], r[FRAME_MAX];
    uint32_t n;

    /* A local stand-in for a peer, and an echo server for the NAT. */
    SOCKET peer, echo;
    struct sockaddr_in a;
    net_socklen_t al = sizeof a;
    net_sock_startup();
    peer = udp_socket(0);
    getsockname(peer, (struct sockaddr *)&a, &al);
    char peers[64];
    snprintf(peers, sizeof peers, "127.0.0.1:%u", ntohs(a.sin_port));
    _putenv_s("RECOMP_NET_PEERS", peers);
    _putenv_s("RECOMP_NET_PORT", "0");
    _putenv_s("RECOMP_NET_DNS", "127.0.0.1");
    if (!net_backend_init("nat")) { puts("FAIL: init"); return 1; }

    /* DHCP DISCOVER -> OFFER for an address from the MAC, valid checksums. */
    uint8_t bootp[244] = {1, 1, 6};
    bootp[4] = 0x12; bootp[5] = 0x34;
    memcpy(bootp + 28, guest, 6);
    wr32(bootp + 236, 0x63825363u);
    bootp[240] = 53; bootp[241] = 1; bootp[242] = 1; bootp[243] = 255;
    n = udp_frame(f, k_bcast, 0, 0xFFFFFFFFu, 68, 67, bootp, sizeof bootp);
    net_backend_send(f, n);
    n = next_frame(r, 500);
    if (n < 14 + 28 + 240 || r[14 + 28] != 2 || r[14 + 28 + 4] != 0x12) { puts("FAIL: no DHCP offer"); return 2; }
    uint32_t yiaddr = rd32(r + 14 + 28 + 16);
    if ((yiaddr & LAN_MASK) != LAN_PREFIX || (yiaddr & 0xFFFFFF00u) == NET_PREFIX ||
        !(yiaddr & 0xFF) || (yiaddr & 0xFF) == 0xFF) { puts("FAIL: offered address"); return 3; }
    {   /* the subnet spans every peer's guest */
        const uint8_t *opt = r + 14 + 28 + 240;
        while (*opt != 255 && *opt != 1) opt += 2 + opt[1];
        if (*opt != 1 || rd32(opt + 2) != LAN_MASK) { puts("FAIL: subnet mask"); return 3; }
    }
    {   /* addresses come from the whole MAC: 64 consoles differing only in byte 3 */
        uint32_t seen[64];
        for (int i = 0; i < 64; i++) {
            uint8_t mac[6] = {0x00, 0x50, 0xF2, (uint8_t)i, 0x02, 0x03};
            seen[i] = guest_address(mac);
            for (int j = 0; j < i; j++)
                if (seen[j] == seen[i]) { puts("FAIL: address collision"); return 3; }
        }
    }
    if (fold(sum16(r + 14, 20, 0)) != 0) { puts("FAIL: IP checksum"); return 4; }
    {
        uint32_t ulen = rd16(r + 14 + 20 + 4);
        uint32_t acc = sum16(r + 14 + 12, 8, 17 + ulen);
        if (fold(sum16(r + 14 + 20, ulen, acc)) != 0) { puts("FAIL: UDP checksum"); return 5; }
    }

    /* ARP for the router. */
    memset(f, 0, 42);
    memcpy(f, k_bcast, 6); memcpy(f + 6, guest, 6); wr16(f + 12, 0x0806);
    wr16(f + 14, 1); wr16(f + 16, 0x0800); f[18] = 6; f[19] = 4; wr16(f + 20, 1);
    memcpy(f + 22, guest, 6); wr32(f + 28, yiaddr); wr32(f + 38, ROUTER_IP);
    net_backend_send(f, 42);
    for (;;) {   /* the broadcast also went to the peer; skip to the reply */
        n = next_frame(r, 500);
        if (!n) { puts("FAIL: no ARP reply"); return 6; }
        if (rd16(r + 12) == 0x0806)
            break;
    }
    if (rd16(r + 20) != 2 || memcmp(r + 22, k_router_mac, 6)) { puts("FAIL: ARP reply"); return 7; }

    /* The peer got both broadcasts, tagged, and a keepalive at once. */
    {
        uint8_t got[4 + FRAME_MAX];
        int frames = 0, keepalives = 0, k;
        net_sock_set_nonblocking(peer, 0);
        net_sock_set_recv_timeout(peer, 500);
        while ((k = recv(peer, (char *)got, sizeof got, 0)) >= 4 && rd32(got) == TUNNEL_MAGIC) {
            if (k == 4) keepalives++;
            else if (k >= 4 + 14) frames++;
            if (frames >= 2 && keepalives) break;
        }
        if (frames < 2) { puts("FAIL: peer did not get the broadcasts"); return 8; }
        if (!keepalives) { puts("FAIL: no keepalive to the listed peer"); return 8; }
    }

    /* UDP out through the NAT to an echo server, and the reply back in. */
    echo = udp_socket(0);
    al = sizeof a;
    getsockname(echo, (struct sockaddr *)&a, &al);
    n = udp_frame(f, k_router_mac, yiaddr, 0x7F000001u, 3074, ntohs(a.sin_port), "ping", 4);
    net_backend_send(f, n);
    {
        char buf[16];
        struct sockaddr_in from;
        net_socklen_t fl = sizeof from;
        int k = -1;
        for (int t = 0; t < 100 && k < 0; t++) {
            k = recvfrom(echo, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
            if (k < 0) Sleep(5);
        }
        if (k != 4 || memcmp(buf, "ping", 4)) { puts("FAIL: NAT did not send"); return 9; }
        sendto(echo, "pong", 4, 0, (struct sockaddr *)&from, fl);
    }
    n = next_frame(r, 500);
    if (n != 14 + 28 + 4 || memcmp(r + 42, "pong", 4) || rd16(r + 14 + 20 + 2) != 3074 ||
        rd32(r + 14 + 16) != yiaddr) { puts("FAIL: NAT reply"); return 10; }

    /* A frame from the peer reaches the guest. */
    {
        uint8_t t[4 + 60] = {0};
        struct sockaddr_in me;
        net_socklen_t ml = sizeof me;
        wr32(t, TUNNEL_MAGIC);
        memcpy(t + 4, guest, 6);
        wr16(t + 4 + 12, 0x88B5);
        getsockname(st.tunnel, (struct sockaddr *)&me, &ml);
        me.sin_addr.s_addr = htonl(0x7F000001u);
        sendto(peer, (const char *)t, sizeof t, 0, (struct sockaddr *)&me, sizeof me);
        n = next_frame(r, 500);
        if (n != 60 || rd16(r + 12) != 0x88B5) { puts("FAIL: tunnel frame not delivered"); return 11; }
    }

    /* A bare keepalive from an unlisted peer teaches its address (hole punching),
     * delivers nothing, and that peer is forgotten once quiet; listed ones stay. */
    {
        uint8_t ka[4];
        struct sockaddr_in me;
        net_socklen_t ml = sizeof me;
        SOCKET other = udp_socket(0);
        int before = st.npeers;
        wr32(ka, TUNNEL_MAGIC);
        getsockname(st.tunnel, (struct sockaddr *)&me, &ml);
        me.sin_addr.s_addr = htonl(0x7F000001u);
        sendto(other, (const char *)ka, sizeof ka, 0, (struct sockaddr *)&me, sizeof me);
        for (int t = 0; t < 100 && st.npeers == before; t++) Sleep(5);
        if (st.npeers != before + 1) { puts("FAIL: keepalive did not add the peer"); return 16; }
        if (next_frame(r, 100)) { puts("FAIL: keepalive delivered a frame"); return 16; }
        AcquireSRWLockExclusive(&st.lock);
        for (int i = 0; i < st.npeers; i++)
            st.peer_seen[i] = GetTickCount() - PEER_EXPIRE_MS - 1;
        st.keepalive_at = 0;
        ReleaseSRWLockExclusive(&st.lock);
        keep_peers_alive();
        if (st.npeers != 1 || !st.peer_listed[0]) { puts("FAIL: quiet learned peers not dropped"); return 17; }
        net_sock_close(other);
    }

    /* Opt-in DNS alias through the actual guest packet path, with a numeric
     * loopback destination: no external DNS or online service is required. */
    {
        static const XboxDnsAlias aliases[] = {{"name.test", "127.0.0.1"}};
        static const uint8_t qry[] = {0xAB, 0xCD, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
            4, 'n', 'a', 'm', 'e', 4, 't', 'e', 's', 't', 0, 0, 1, 0, 1};
        uint8_t answer[600];
        if (dns_alias_answer(qry,sizeof qry,answer)) { puts("FAIL: default DNS alias"); return 12; }
        net_backend_set_dns_aliases(aliases,1);
        if (dns_alias_answer(qry,16,answer)) { puts("FAIL: truncated DNS query"); return 13; }
        uint8_t unknown[sizeof qry]; memcpy(unknown,qry,sizeof qry); unknown[13]='x';
        if (dns_alias_answer(unknown,sizeof unknown,answer)) { puts("FAIL: unrelated DNS query"); return 13; }
        unknown[13]='n'; unknown[sizeof unknown-3]=28; /* AAAA */
        if (dns_alias_answer(unknown,sizeof unknown,answer)) { puts("FAIL: non-A DNS query"); return 13; }
        n = udp_frame(f, k_router_mac, yiaddr, DNS_IP, 1053, 53, qry, sizeof qry);
        net_backend_send(f, n);
        n = next_frame(r, 500);
        const uint8_t *d = r + 42;
        if (n != 42 + sizeof qry + 16 || rd16(d) != 0xABCD || rd16(d + 2) != 0x8180 ||
            rd16(r + 36) != 1053 || rd16(d + 6) != 1 || rd32(r + n - 4) != 0x7F000001u) {
            puts("FAIL: no synthetic DNS answer"); return 14;
        }
        net_backend_set_dns_aliases(NULL,0);
        if (dns_alias_answer(qry,sizeof qry,answer)) { puts("FAIL: DNS alias clear"); return 15; }
    }

    puts("net: ok");
    return 0;
}
