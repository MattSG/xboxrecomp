/*
 * nvnet.c -- the MCPX nForce Ethernet controller at 0xFEF00000.
 *
 * Ported from xemu's hw/xbox/mcpx/nvnet/nvnet.c:
 *   Copyright (c) 2013 espes
 *   Copyright (c) 2015-2025 Matt Borgerson
 * Licensed under the GNU Lesser General Public License 2.1 or later
 * (LICENSES/LGPL-2.1.txt).
 *
 * A title's XNet is linked into its image and drives these registers
 * itself: a TX and an RX ring of 8-byte descriptors in contiguous memory, a
 * kick to transmit, an interrupt when a frame lands. Without the model the
 * registers read back from plain RAM, nothing is ever sent, DHCP never
 * answers and the title's network readiness never comes.
 *
 * The register semantics follow xemu. What differs is the plumbing: the
 * aperture is trapped like the USB controllers' (mmio_decode.h), DMA goes
 * through the bus resolver the OHCI model uses, and frames go to and come
 * from net_backend.c instead of a QEMU netdev. Received frames are queued
 * by the backend's thread and moved into the RX ring here, under the lock,
 * by the NIC thread, which also calls the title's interrupt routine.
 */
#include "net.h"
#include "../platform/mmio_decode.h"
#include "../kernel/xbox_memory_layout.h"
#include "../kernel/kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);
typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern int  xbox_worker_stack_alloc(void);
extern void xbox_worker_stack_free(int slot);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
extern __declspec(thread) uint32_t g_eax, g_ecx, g_edx, g_esp;
extern __declspec(thread) uint32_t g_ebx, g_esi, g_edi;
extern __declspec(thread) uint32_t g_fs_base;
extern uint32_t xbox_AllocThreadTib(void);

#define NIC_BASE    0xFEF00000u
#define NIC_SIZE    0x1000u     /* the page; the registers are the first 0x400 */
#define NIC_REGS    0x400u
#define NIC_VECTOR  4           /* HalGetInterruptVector(4): the NIC's IRQ */

/* Registers (offsets from forcedeth's names, as xemu uses them). */
#define R_IRQ_STATUS        0x000
#define   IRQ_RX              0x00000002u
#define   IRQ_TX              0x00000010u
#define   IRQ_MIIEVENT        0x00000040u
#define R_IRQ_MASK          0x004
#define R_TRANSMITTER_CTL   0x084
#define R_PACKET_FILTER     0x08C
#define   FILTER_MYADDR       0x00000020u
#define R_RECEIVER_CTL      0x094
#define R_MAC_ADDR_A        0x0A8
#define R_MAC_ADDR_B        0x0AC
#define R_MCAST_ADDR_A      0x0B0
#define R_MCAST_ADDR_B      0x0B4
#define R_MCAST_MASK_A      0x0B8
#define R_MCAST_MASK_B      0x0BC
#define R_TX_RING           0x100
#define R_RX_RING           0x104
#define R_RING_SIZE         0x108
#define R_TX_CUR_DESC       0x11C
#define R_RX_CUR_DESC       0x120
#define R_SETUP5            0x130
#define R_TX_NEXT_DESC      0x134
#define R_RX_NEXT_DESC      0x138
#define R_TX_RX_CTL         0x144
#define   TXRX_KICK           0x00000001u
#define   TXRX_BIT1           0x00000002u
#define   TXRX_BIT2           0x00000004u
#define   TXRX_IDLE           0x00000008u
#define   TXRX_RESET          0x00000010u
#define R_MII_STATUS        0x180
#define   MII_LINKCHANGE      0x00000008u
#define R_ADAPTER_CTL       0x188
#define   ADAPTER_LINKUP      0x00000004u
#define R_MDIO_ADDR         0x190
#define   MDIO_INUSE          0x00008000u
#define   MDIO_WRITE          0x00000400u
#define R_MDIO_DATA         0x194

#define NV_TX_LASTPACKET    (1u << 0)
#define NV_TX_VALID         (1u << 15)
#define NV_TX_ERRORS        ((1u << 3) | (1u << 10) | (1u << 11) | (1u << 12) | \
                             (1u << 13) | (1u << 14))
#define NV_RX_DESCRIPTORVALID (1u << 0)
#define NV_RX_BIT4          (1u << 4)
#define NV_RX_AVAIL         (1u << 15)

/* The PHY at MDIO address 1: auto-negotiated 100 Mbit full duplex, link up. */
#define PHY_ADDR  1
enum { MII_BMCR, MII_BMSR, MII_PHYID1, MII_PHYID2, MII_ANAR, MII_ANLPAR };
static const uint32_t phy_init[6] = {
    [MII_BMCR] = 0x1100,               /* full duplex, auto-negotiation on */
    [MII_BMSR] = 0x0008 | 0x0020 | 0x0004, /* can autoneg, done, link */
    [MII_ANAR] = 0x01E1,
    [MII_ANLPAR] = 0x41E1,             /* + acknowledge */
};

#define BUF_SIZE  (1500 + 128)

/* Flushed: stderr is buffered when redirected, and a killed run keeps nothing. */
#define NIC_LOG(...) do { fprintf(stderr, __VA_ARGS__); fflush(stderr); } while (0)

static struct {
    CRITICAL_SECTION lock;
    uint8_t regs[NIC_REGS];
    uint32_t phy[6];
    uint8_t tx_buf[BUF_SIZE];
    uint32_t tx_len;
    int enabled, link_announced;
    unsigned tx_frames, rx_frames, rx_dropped, decode_fail, irqs;
} s;

/* ---- guest memory: the bus resolver the OHCI model uses ---------------- */

#define CONTIG_BASE 0x80000000u
#define CONTIG_SIZE (64u * 1024u * 1024u)

static uint8_t *bus_ptr(uint32_t addr, uint32_t bytes)
{
    /* XNet's rings and buffers are MmAllocateContiguousMemory, handed over as
     * physical addresses -- which can fall inside the image's virtual range
     * (its rings sit at physical 0x47F000, under BINKDATA). Unlike the USB
     * driver it never points the controller at its own statics, so the
     * allocator's answer wins. */
    size_t mapped = xbox_GetMappedSize();
    if (addr < CONTIG_BASE && xbox_ContiguousRangeAllocated(addr, bytes))
        addr += CONTIG_BASE;
    if (addr >= CONTIG_BASE
            ? (uint64_t)addr + bytes > (uint64_t)CONTIG_BASE + CONTIG_SIZE
            : (addr == 0 || (uint64_t)addr + bytes > mapped))
        return NULL;
    return (uint8_t *)xbox_GetMemoryOffset() + addr;
}

/* ---- registers --------------------------------------------------------- */

static uint32_t get(uint32_t off) { uint32_t v; memcpy(&v, s.regs + off, 4); return v; }
static void put(uint32_t off, uint32_t v) { memcpy(s.regs + off, &v, 4); }
static void or_(uint32_t off, uint32_t v) { put(off, get(off) | v); }
static void and_(uint32_t off, uint32_t v) { put(off, get(off) & v); }

static int irq_pending(void) { return (get(R_IRQ_STATUS) & get(R_IRQ_MASK)) != 0; }

static void set_irq(uint32_t bits) { or_(R_IRQ_STATUS, bits); }

static int link_up(void) { return (s.phy[MII_BMSR] & 0x0004) != 0; }
static int dma_enabled(void) { return (get(R_TX_RX_CTL) & TXRX_BIT2) == 0; }

typedef struct { uint32_t buf; uint16_t len, flags; } Desc;

static int load_desc(uint32_t at, Desc *d)
{
    uint8_t *p = bus_ptr(at, 8);
    if (!p)
        return 0;
    memcpy(&d->buf, p, 4);
    memcpy(&d->len, p + 4, 2);
    memcpy(&d->flags, p + 6, 2);
    return 1;
}

static void store_desc(uint32_t at, const Desc *d)
{
    uint8_t *p = bus_ptr(at, 8);
    if (!p)
        return;
    memcpy(p, &d->buf, 4);
    memcpy(p + 4, &d->len, 2);
    memcpy(p + 6, &d->flags, 2);
}

/* The current descriptor of a ring: the next one, wrapped into the ring. */
static uint32_t ring_current(uint32_t base_reg, uint32_t next_reg, uint32_t cur_reg, unsigned n)
{
    uint32_t base = get(base_reg), cur = get(next_reg);
    if (cur < base || cur + 8 > base + n * 8)
        cur = base;
    put(cur_reg, cur);
    return cur;
}

static void ring_advance(uint32_t base_reg, uint32_t next_reg, uint32_t cur_reg, unsigned n)
{
    uint32_t base = get(base_reg), next = get(cur_reg) + 8;
    put(next_reg, next >= base + n * 8 ? base : next);
}

static unsigned tx_ring_n(void) { return (get(R_RING_SIZE) & 0xFFFF) + 1; }
static unsigned rx_ring_n(void) { return (get(R_RING_SIZE) >> 16) + 1; }

static void transmit(void)
{
    int sent = 0;
    if (!(get(R_TRANSMITTER_CTL) & 1) || !dma_enabled() || !link_up())
        return;
    and_(R_TX_RX_CTL, ~TXRX_IDLE);
    for (unsigned i = 0; i < tx_ring_n(); i++) {
        uint32_t at = ring_current(R_TX_RING, R_TX_NEXT_DESC, R_TX_CUR_DESC, tx_ring_n());
        Desc d;
        if (!load_desc(at, &d) || !(d.flags & NV_TX_VALID))
            break;
        uint32_t len = (uint32_t)d.len + 1;
        uint8_t *src = bus_ptr(d.buf, len);
        if (src && s.tx_len + len <= sizeof s.tx_buf) {
            memcpy(s.tx_buf + s.tx_len, src, len);
            s.tx_len += len;
        }
        int last = (d.flags & NV_TX_LASTPACKET) != 0;
        if (last) {
            net_backend_send(s.tx_buf, s.tx_len);
            s.tx_frames++;
            s.tx_len = 0;
            sent = 1;
        }
        d.flags &= ~(NV_TX_VALID | NV_TX_ERRORS);
        store_desc(at, &d);
        ring_advance(R_TX_RING, R_TX_NEXT_DESC, R_TX_CUR_DESC, tx_ring_n());
        if (last)
            break;
    }
    or_(R_TX_RX_CTL, TXRX_IDLE);
    if (sent)
        set_irq(IRQ_TX);
}

static int accept(const uint8_t *f, uint32_t n)
{
    static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t a[2], m[2];
    if (n < 14)
        return 0;
    if (!memcmp(f, bcast, 6) || !(get(R_PACKET_FILTER) & FILTER_MYADDR))
        return 1;
    a[0] = get(R_MCAST_ADDR_A); a[1] = get(R_MCAST_ADDR_B);
    if (memcmp(a, bcast, 6)) {
        memcpy(m, f, 6);
        m[0] &= get(R_MCAST_MASK_A); m[1] &= get(R_MCAST_MASK_B);
        if (!memcmp(m, a, 6))
            return 1;
    }
    a[0] = get(R_MAC_ADDR_A); a[1] = get(R_MAC_ADDR_B);
    return !memcmp(f, a, 6);
}

/* One frame into the RX ring. 0 when the guest has no buffer free yet. */
static int receive(const uint8_t *f, uint32_t n)
{
    Desc d;
    if (!(get(R_RECEIVER_CTL) & 1) || !dma_enabled() || !link_up())
        return 0;
    uint32_t at = ring_current(R_RX_RING, R_RX_NEXT_DESC, R_RX_CUR_DESC, rx_ring_n());
    if (!load_desc(at, &d) || !(d.flags & NV_RX_AVAIL))
        return 0;
    if ((uint32_t)d.len + 1 < n) { /* xemu asserts; drop instead */
        s.rx_dropped++;
        return 1;
    }
    uint8_t *dst = bus_ptr(d.buf, n);
    if (!dst) {
        s.rx_dropped++;
        return 1;
    }
    and_(R_TX_RX_CTL, ~TXRX_IDLE);
    memcpy(dst, f, n);
    d.len = (uint16_t)n;
    d.flags = NV_RX_BIT4 | NV_RX_DESCRIPTORVALID;
    store_desc(at, &d);
    set_irq(IRQ_RX);
    ring_advance(R_RX_RING, R_RX_NEXT_DESC, R_RX_CUR_DESC, rx_ring_n());
    or_(R_TX_RX_CTL, TXRX_IDLE);
    s.rx_frames++;
    return 1;
}

/* RECOMP_NIC_TRACE: the first register accesses, with their values. */
static void trace(const char *op, uint32_t off, uint32_t v)
{
    static int on = -1;
    static unsigned n, last_off = ~0u, repeats;
    if (on < 0)
        on = getenv("RECOMP_NIC_TRACE") != NULL;
    if (!on || n >= 400)
        return;
    if (off == last_off && op[0] == 'r' && ++repeats > 3)
        return; /* a poll loop: show it once */
    if (off != last_off)
        repeats = 0;
    last_off = off;
    n++;
    NIC_LOG("  [NIC] %s +0x%03X %08X\n", op, off, v);
}

static uint64_t nic_read(void *dev, uint32_t off, int size)
{
    uint64_t v = 0;
    (void)dev;
    if (off + (uint32_t)size > NIC_REGS)
        return 0;
    EnterCriticalSection(&s.lock);
    memcpy(&v, s.regs + off, (size_t)size);
    LeaveCriticalSection(&s.lock);
    trace("read ", off, (uint32_t)v);
    return v;
}

static void nic_write(void *dev, uint32_t off, uint64_t val, int size)
{
    uint32_t v = (uint32_t)val;
    (void)dev;
    if (off + (uint32_t)size > NIC_REGS)
        return;
    trace("write", off, v);
    EnterCriticalSection(&s.lock);
    switch (off) {
    case R_MDIO_ADDR: {
        unsigned addr = (v >> 5) & 0x1F, reg = v & 0x1F;
        put(off, v);
        if (v & MDIO_WRITE) {
            if (addr == PHY_ADDR && reg < 6)
                s.phy[reg] = get(R_MDIO_DATA) & 0xFFFF;
        } else {
            put(R_MDIO_DATA, addr == PHY_ADDR ? (reg < 6 ? s.phy[reg] : 0) : 0xFFFFFFFFu);
        }
        and_(R_MDIO_ADDR, ~MDIO_INUSE);
        break;
    }
    case R_TX_RX_CTL:
        put(off, (get(off) & (v | TXRX_IDLE)) | (v & ~TXRX_IDLE));
        if (v & TXRX_KICK)
            transmit();
        if (v & TXRX_RESET) {
            put(R_TX_CUR_DESC, get(R_TX_RING)); put(R_TX_NEXT_DESC, get(R_TX_RING));
            put(R_RX_CUR_DESC, get(R_RX_RING)); put(R_RX_NEXT_DESC, get(R_RX_RING));
            s.tx_len = 0;
        }
        if (v & TXRX_BIT1)
            put(R_IRQ_STATUS, 0);
        else if (v == 0)
            put(R_SETUP5, 0x80000000u);   /* forcedeth waits for bit 31 */
        break;
    case R_IRQ_STATUS:
    case R_MII_STATUS: { /* write one to clear */
        uint32_t cur = 0;
        memcpy(&cur, s.regs + off, (size_t)size);
        cur &= ~v;
        memcpy(s.regs + off, &cur, (size_t)size);
        break;
    }
    default:
        memcpy(s.regs + off, &v, (size_t)size);
        break;
    }
    LeaveCriticalSection(&s.lock);
}

/* ---- the interrupt ----------------------------------------------------- */

static void call_isr(void)
{
    uint32_t kint = xbox_GetConnectedInterrupt(NIC_VECTOR);
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    if (!kint)
        return;
    uint32_t routine = *(uint32_t *)(mem + kint), context = *(uint32_t *)(mem + kint + 4);
    recomp_func_t fn = routine ? recomp_lookup(routine) : NULL;
    int slot = fn ? xbox_worker_stack_alloc() : -1;
    if (slot < 0) {
        static int warned;
        if (!warned++)
            NIC_LOG("  [NIC] cannot call ISR 0x%08X (%s)\n", routine,
                    fn ? "no worker stack" : "no translation");
        return;
    }
    g_esp = XBOX_WORKER_STACK_TOP(slot);
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = context;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = kint;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0xDEADBEEFu;
    { int irql = xbox_IrqlEnterInterrupt(16); fn(); xbox_IrqlLeaveInterrupt(irql); }
    xbox_worker_stack_free(slot);
    if (s.irqs++ < 3)
        NIC_LOG("  [NIC] interrupt delivered to 0x%08X -> %s\n", routine,
                (g_eax & 1) ? "claimed" : "declined");
}

/* Moves received frames into the ring and raises the line while it is up.
 * 1 ms: a frame waits at most that long for its interrupt. */
static DWORD WINAPI nic_thread(LPVOID unused)
{
    static uint8_t frame[BUF_SIZE];
    (void)unused;
    g_fs_base = xbox_AllocThreadTib();
    if (!g_fs_base) {
        NIC_LOG("  [NIC] no TIB for the NIC thread; no interrupts\n");
        return 0;
    }
    timeBeginPeriod(1);
    for (;;) {
        uint32_t n;
        int pending;
        net_backend_wait(1);
        EnterCriticalSection(&s.lock);
        while ((n = net_backend_peek(frame, sizeof frame)) != 0) {
            if (!accept(frame, n)) {
                net_backend_pop();
                continue;
            }
            if (!receive(frame, n))
                break;   /* no free buffer: keep it until the guest frees one */
            net_backend_pop();
        }
        /* Auto-negotiation completing: xemu's autoneg_done -> set_link_up
         * reports the link through an MII interrupt, which is what sends
         * XNet to read the link state and start DHCP. Once, when the driver
         * listens for it and has a routine connected to hear it. */
        if (!s.link_announced && (get(R_IRQ_MASK) & IRQ_MIIEVENT) && link_up() &&
            xbox_GetConnectedInterrupt(NIC_VECTOR)) {
            s.link_announced = 1;
            or_(R_MII_STATUS, MII_LINKCHANGE);
            set_irq(IRQ_MIIEVENT);
        }
        pending = irq_pending();
        LeaveCriticalSection(&s.lock);
        if (pending)
            call_isr();
    }
}

void xbox_NicInit(void)
{
    const char *mode = getenv("RECOMP_NET");
    ptrdiff_t off = xbox_GetMemoryOffset();
    DWORD old;
    if (!mode || !*mode || !strcmp(mode, "0"))
        return;
    if (!off || !net_backend_init(mode)) {
        NIC_LOG("  NIC: backend '%s' unavailable; network off\n", mode);
        return;
    }
    InitializeCriticalSection(&s.lock);
    memcpy(s.phy, phy_init, sizeof s.phy);
    or_(R_TX_RX_CTL, TXRX_IDLE);
    or_(R_ADAPTER_CTL, ADAPTER_LINKUP);
    if (!VirtualProtect((LPVOID)((uintptr_t)off + NIC_BASE), NIC_SIZE, PAGE_NOACCESS, &old)) {
        NIC_LOG("  NIC: cannot trap 0x%08X (error %lu); network off\n",
                NIC_BASE, GetLastError());
        return;
    }
    s.enabled = 1;
    HANDLE th = CreateThread(NULL, 0, nic_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    NIC_LOG("  NIC: nForce Ethernet at 0x%08X, backend %s\n", NIC_BASE, mode);
    fflush(stderr);
}

int xbox_NicOwnsAddress(uint32_t va)
{
    return s.enabled && va >= NIC_BASE && va < NIC_BASE + NIC_SIZE;
}

int xbox_NicHandleMmio(void *ctx, uint32_t va)
{
    int ok = mmio_emulate((PCONTEXT)ctx, va - NIC_BASE, NULL, nic_read, nic_write);
    if (!ok && s.decode_fail++ < 20) {
        const uint8_t *ip = (const uint8_t *)((PCONTEXT)ctx)->Rip;
        NIC_LOG("  [NIC] undecoded access at +0x%03X: %02X %02X %02X %02X\n",
                va - NIC_BASE, ip[0], ip[1], ip[2], ip[3]);
        fflush(stderr);
    }
    return ok;
}

void xbox_NicReport(void)
{
    if (s.enabled)
        NIC_LOG("  NIC: %u frames out, %u in, %u dropped, %u interrupts\n",
                s.tx_frames, s.rx_frames, s.rx_dropped, s.irqs);
}

#else
void xbox_NicInit(void) {}
int xbox_NicOwnsAddress(uint32_t va) { (void)va; return 0; }
int xbox_NicHandleMmio(void *ctx, uint32_t va) { (void)ctx; (void)va; return 0; }
void xbox_NicReport(void) {}
#endif
