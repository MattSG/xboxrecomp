/* mcpx_apu_phys from the real apu_core.c: each mapping resolves to its own
 * storage, an ordinary heap address inside the guest RAM map resolves to
 * itself (a title streams voices from heap pages), and an address past the map is
 * refused rather than wrapped into low guest RAM (the old a & 0x03FFFFFF
 * fallback). */
#include "../../src/apu/apu_core.c"
#include <stdio.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "APU phys check failed at line %d: %s\n", __LINE__, #condition); \
    exit(1); } } while (0)

/* Kernel mappings, as [begin, end) ranges set by main. */
static uint32_t contig_begin, contig_end, reserve_begin, reserve_end;
int xbox_ContiguousRangeAllocated(uint32_t offset, uint32_t size)
{
    return offset >= contig_begin && offset + size <= contig_end;
}
int xbox_ReserveRangeAllocated(uint32_t va)
{
    return va >= reserve_begin && va < reserve_end;
}
uint32_t g_xbox_image_lo, g_xbox_image_hi;
size_t g_xbox_map_size, g_xbox_total_ram;

/* apu_core.c is compiled whole, so its other entry points must still link.
 * None is reached by mcpx_apu_phys: each function stub aborts if called, so an
 * unexpected call fails the test instead of crashing or silently succeeding.
 * The register globals are data the test never touches. */
static void unexpected_call(const char *name)
{
    fprintf(stderr, "apu_phys_test: unexpected call to %s\n", name);
    abort();
}
void mcpx_apu_dsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME], int div)
{ (void)d; (void)mixbins; (void)div; unexpected_call("mcpx_apu_dsp_frame"); }
void mcpx_apu_dsp_init(MCPXAPUState *d) { (void)d; unexpected_call("mcpx_apu_dsp_init"); }
void mcpx_apu_dsp_pcm_consumed(MCPXAPUState *d) { (void)d; unexpected_call("mcpx_apu_dsp_pcm_consumed"); }
uint64_t mcpx_apu_dsp_read(MCPXAPUState *d, bool gp, uint32_t addr, unsigned size)
{ (void)d; (void)gp; (void)addr; (void)size; unexpected_call("mcpx_apu_dsp_read"); return 0; }
void mcpx_apu_dsp_write(MCPXAPUState *d, bool gp, uint32_t addr, uint64_t value, unsigned size)
{ (void)d; (void)gp; (void)addr; (void)value; (void)size; unexpected_call("mcpx_apu_dsp_write"); }
void mcpx_apu_update_dsp_preference(MCPXAPUState *d) { (void)d; unexpected_call("mcpx_apu_update_dsp_preference"); }
void mcpx_apu_vp_finalize(MCPXAPUState *d) { (void)d; unexpected_call("mcpx_apu_vp_finalize"); }
void mcpx_apu_vp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{ (void)d; (void)mixbins; unexpected_call("mcpx_apu_vp_frame"); }
void mcpx_apu_vp_init(MCPXAPUState *d) { (void)d; unexpected_call("mcpx_apu_vp_init"); }
void mcpx_apu_vp_reset(MCPXAPUState *d) { (void)d; unexpected_call("mcpx_apu_vp_reset"); }
void dsp_reset(DSPState *dsp) { (void)dsp; unexpected_call("dsp_reset"); }
uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size)
{ (void)opaque; (void)addr; (void)size; unexpected_call("mcpx_apu_vp_read"); return 0; }
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size)
{ (void)opaque; (void)addr; (void)val; (void)size; unexpected_call("mcpx_apu_vp_write"); }
apu_guest_fn recomp_lookup(uint32_t xbox_va) { (void)xbox_va; unexpected_call("recomp_lookup"); return NULL; }
int xa2_init(void) { unexpected_call("xa2_init"); return 0; }
int xa2_is_active(void) { unexpected_call("xa2_is_active"); return 0; }
void xa2_shutdown(void) { unexpected_call("xa2_shutdown"); }
int xa2_submit_samples(const int16_t *samples, int num_samples)
{ (void)samples; (void)num_samples; unexpected_call("xa2_submit_samples"); return 0; }
uint32_t xbox_AllocThreadTib(void) { unexpected_call("xbox_AllocThreadTib"); return 0; }
uint32_t xbox_GetConnectedInterrupt(uint32_t vector) { (void)vector; unexpected_call("xbox_GetConnectedInterrupt"); return 0; }
int xbox_IrqlEnterInterrupt(int level) { (void)level; unexpected_call("xbox_IrqlEnterInterrupt"); return 0; }
void xbox_IrqlLeaveInterrupt(int saved) { (void)saved; unexpected_call("xbox_IrqlLeaveInterrupt"); }
int xbox_worker_stack_alloc(void) { unexpected_call("xbox_worker_stack_alloc"); return -1; }
void xbox_worker_stack_free(int slot) { (void)slot; unexpected_call("xbox_worker_stack_free"); }
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_fs_base;

int main(void)
{
    static uint8_t ram[0x2000];
    ram[0] = 0xAB; /* what a refused 0x04000000 would alias */
    g_apu_ram_ptr = ram;
    contig_begin = 0x1000; contig_end = 0x3000;
    reserve_begin = 0x06000000u; reserve_end = 0x07000000u;
    g_xbox_image_lo = 0x10000; g_xbox_image_hi = 0x20000;
    g_xbox_total_ram = 0x04000000u; g_xbox_map_size = 0x08000000u; /* 128 MB map */

    /* Mapped addresses: pointers only, since the window and reservations
     * are not backed by this buffer. */
    CHECK(mcpx_apu_phys(0x1000) == ram + 0x80001000u);
    CHECK(mcpx_apu_phys(0x80001000u) == ram + 0x80001000u);
    CHECK(mcpx_apu_phys(0x10040) == ram + 0x10040);
    CHECK(mcpx_apu_phys(0x06000010u) == ram + 0x06000010u);

    /* Heap pages inside the map are identity-mapped: a title voice buffer at
     * 0x01191EA0 and one above 64 MB both resolve to themselves. */
    CHECK(mcpx_apu_phys(0x01191EA0u) == ram + 0x01191EA0u);
    CHECK(mcpx_apu_phys(0x05000040u) == ram + 0x05000040u);

    /* Past the 128 MB map: 0x08000000 would wrap to ram[0] and 0x0A000000 to
     * ram + 0x02000000. */
    uint8_t *p = mcpx_apu_phys(0x08000000u);
    CHECK(p != ram && p[0] == 0);
    p[0] = 0x5A; /* a refused write goes nowhere */
    CHECK(ram[0] == 0xAB);
    CHECK(mcpx_apu_phys(0x0A000000u) != ram + 0x02000000u);

    printf("APU phys: mapped windows resolve to their own storage; unmapped refused\n");
    return 0;
}
