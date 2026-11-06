/* The NIC's descriptor rings through the real nvnet.c. A next-descriptor
 * pointer outside its ring stops that ring: nothing is transferred and the
 * pointer is left as the driver wrote it, rather than reset to the base. The
 * legitimate wrap past the last descriptor still happens. */
#if defined(_WIN32)
#include <winsock2.h>
#endif
/* nvnet.c has its own static accept() (the NIC), which Winsock also declares. */
#define accept nic_accept
#include "../../src/net/nvnet.c"
#include <stdio.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "NIC ring check line %d: %s\n", __LINE__, #condition); \
    exit(1); } } while (0)

static uint8_t guest[0x4000]; /* guest physical memory, from address 0 */
static unsigned sends;
static uint8_t sent_frame[60];
#define RAM_ID_BASE 0x04000000u

ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)guest; }
size_t xbox_GetMappedSize(void) { return sizeof(guest); }
int xbox_PhysicalAddressResolve(uint32_t physical, uint32_t bytes, uint32_t *va)
{
    if (!bytes || physical < RAM_ID_BASE ||
        (uint64_t)physical + bytes > RAM_ID_BASE + sizeof(guest)) return 0;
    *va = physical - RAM_ID_BASE;
    return 1;
}
int xbox_ContiguousRangeAllocated(uint32_t offset, uint32_t size)
{
    (void)offset; (void)size;
    return 0;
}
void net_backend_send(const uint8_t *frame, uint32_t len)
{
    CHECK(len == sizeof(sent_frame));
    memcpy(sent_frame, frame, len);
    sends++;
}

/* nvnet.c is compiled whole, so its other entry points must still link. The
 * ring path never reaches these: each stub aborts if it is called, so an
 * unexpected call fails the test. The register globals are data the test never
 * touches. timeBeginPeriod is the real winmm function (linked in CMake). */
static void unexpected_call(const char *name)
{
    fprintf(stderr, "nvnet_ring_test: unexpected call to %s\n", name);
    abort();
}
int net_backend_init(const char *mode) { (void)mode; unexpected_call("net_backend_init"); return 0; }
uint32_t net_backend_peek(uint8_t *buf, uint32_t cap)
{ (void)buf; (void)cap; unexpected_call("net_backend_peek"); return 0; }
void net_backend_pop(void) { unexpected_call("net_backend_pop"); }
void net_backend_wait(unsigned ms) { (void)ms; unexpected_call("net_backend_wait"); }
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; unexpected_call("recomp_lookup"); return NULL; }
uint32_t xbox_AllocThreadTib(void) { unexpected_call("xbox_AllocThreadTib"); return 0; }
uint32_t xbox_GetConnectedInterrupt(uint32_t vector) { (void)vector; unexpected_call("xbox_GetConnectedInterrupt"); return 0; }
int xbox_IrqlEnterInterrupt(int level) { (void)level; unexpected_call("xbox_IrqlEnterInterrupt"); return 0; }
void xbox_IrqlLeaveInterrupt(int saved) { (void)saved; unexpected_call("xbox_IrqlLeaveInterrupt"); }
int xbox_worker_stack_alloc(void) { unexpected_call("xbox_worker_stack_alloc"); return -1; }
void xbox_worker_stack_free(int slot) { (void)slot; unexpected_call("xbox_worker_stack_free"); }
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_fs_base;

static void desc_put(uint32_t at, uint32_t buf, uint16_t len, uint16_t flags)
{
    memcpy(guest + at, &buf, 4);
    memcpy(guest + at + 4, &len, 2);
    memcpy(guest + at + 6, &flags, 2);
}

static uint16_t desc_flags(uint32_t at)
{
    uint16_t flags;
    memcpy(&flags, guest + at + 6, 2);
    return flags;
}

int main(void)
{
    uint8_t frame[60];
    memset(frame, 0xC3, sizeof(frame));
    memset(guest, 0x11, sizeof(guest));

    /* RX: a 4-descriptor ring at 0x1000 and a receiver with DMA on. */
    put(R_RING_SIZE, 3u << 16);
    put(R_RX_RING, 0x1000);
    put(R_RECEIVER_CTL, 1);
    put(R_TX_RX_CTL, 0);
    s.phy[MII_BMSR] = 0x0004;
    for (uint32_t i = 0; i < 4; i++)
        desc_put(0x1000 + i * 8, 0x2000 + i * 0x100, 1500, NV_RX_AVAIL);
    put(R_RX_NEXT_DESC, 0x1000);
    put(R_RX_CUR_DESC, 0x1000);

    /* Control: a good pointer delivers into the first descriptor. */
    CHECK(receive(frame, sizeof(frame)) == 1);
    CHECK(!memcmp(guest + 0x2000, frame, sizeof(frame)));
    CHECK(get(R_RX_NEXT_DESC) == 0x1008);

    /* Below the ring: refused. Nothing is written, the pointer is left alone,
     * and the descriptor at the base is not consumed. */
    for (uint32_t i = 0; i < 4; i++) desc_put(0x1000 + i * 8, 0x2000 + i * 0x100, 1500, NV_RX_AVAIL);
    memset(guest + 0x2000, 0x11, sizeof(frame));
    put(R_RX_NEXT_DESC, 0x0800);
    CHECK(receive(frame, sizeof(frame)) == 0);
    CHECK(get(R_RX_NEXT_DESC) == 0x0800);
    CHECK(desc_flags(0x1000) == NV_RX_AVAIL);
    CHECK(guest[0x2000] == 0x11);

    /* Past the last descriptor: also refused, not wrapped. */
    put(R_RX_NEXT_DESC, 0x1020);
    CHECK(receive(frame, sizeof(frame)) == 0);
    CHECK(get(R_RX_NEXT_DESC) == 0x1020);
    CHECK(desc_flags(0x1000) == NV_RX_AVAIL);

    /* The legitimate wrap: the last descriptor hands over to the base. */
    put(R_RX_NEXT_DESC, 0x1018);
    CHECK(receive(frame, sizeof(frame)) == 1);
    CHECK(get(R_RX_NEXT_DESC) == 0x1000);

    /* TX: a valid one-descriptor packet at 0x1100 is sent with a good pointer. */
    put(R_TX_RING, 0x1100);
    put(R_TRANSMITTER_CTL, 1);
    desc_put(0x1100, 0x2200, 59, NV_TX_VALID | NV_TX_LASTPACKET);
    memset(guest + 0x2200, 0x22, 60);
    put(R_TX_NEXT_DESC, 0x1100);
    transmit();
    CHECK(sends == 1);

    /* TX with a bad pointer sends nothing and leaves the descriptor as it is. */
    desc_put(0x1100, 0x2200, 59, NV_TX_VALID | NV_TX_LASTPACKET);
    put(R_TX_NEXT_DESC, 0x0800);
    transmit();
    CHECK(sends == 1);
    CHECK(get(R_TX_NEXT_DESC) == 0x0800);
    CHECK(desc_flags(0x1100) == (NV_TX_VALID | NV_TX_LASTPACKET));

    /* Runtime physical IDs must resolve for descriptors and payloads, on
     * both DMA directions. Raw-VA cases above keep their old behavior. */
    put(R_RX_RING, RAM_ID_BASE + 0x1000);
    put(R_RX_NEXT_DESC, RAM_ID_BASE + 0x1000);
    desc_put(0x1000, RAM_ID_BASE + 0x2000, 1500, NV_RX_AVAIL);
    memset(guest + 0x2000, 0, sizeof(frame));
    CHECK(receive(frame, sizeof(frame)) == 1);
    CHECK(!memcmp(guest + 0x2000, frame, sizeof(frame)));
    CHECK(desc_flags(0x1000) == (NV_RX_BIT4 | NV_RX_DESCRIPTORVALID));
    CHECK(get(R_RX_NEXT_DESC) == RAM_ID_BASE + 0x1008);
    put(R_TX_RING, RAM_ID_BASE + 0x1100);
    put(R_TX_NEXT_DESC, RAM_ID_BASE + 0x1100);
    desc_put(0x1100, RAM_ID_BASE + 0x2200, 59, NV_TX_VALID | NV_TX_LASTPACKET);
    transmit();
    CHECK(sends == 2);
    CHECK(!memcmp(sent_frame, guest + 0x2200, sizeof(sent_frame)));
    CHECK(!(desc_flags(0x1100) & NV_TX_VALID));
    CHECK(bus_ptr(RAM_ID_BASE + sizeof(guest) - 4, 8) == NULL);
    printf("NIC rings: pointer bounds, wrap and runtime physical DMA IDs passed\n");
    return 0;
}

/* Callback paths are forbidden by this focused fixture. */
uint32_t g_xbox_worker_stack_base;
