/* What nv2a_pb_exec.c reaches outside the backend interface, for tests that
 * drive the executor through Nv2aBackend: the D3D renderer stays off, guest
 * memory is one flat buffer (physical P is guest P), and nothing is listening
 * for semaphores or NOPs. */
#include <stdint.h>
#include <stddef.h>

uint32_t g_xbox_kernel_data_base;

int nv2a_d3d_init(void) { return 0; }
int nv2a_d3d_widescreen(void) { return 0; }
void nv2a_d3d_clear(const void *st, uint32_t flags, uint32_t color, uint32_t zstencil,
                    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{ (void)st; (void)flags; (void)color; (void)zstencil; (void)x0; (void)y0; (void)x1; (void)y1; }
void nv2a_d3d_draw_ub(void) {}
void nv2a_d3d_flip_present(uint32_t va, uint32_t pitch, int now) { (void)va; (void)pitch; (void)now; }
void nv2a_d3d_zpass_enable(int on) { (void)on; }
void nv2a_d3d_zpass_clear(void) {}
uint32_t nv2a_d3d_zpass_read(void) { return 0; }

int xbox_ContiguousRangeAllocated(uint32_t physical_offset, uint32_t size)
{ (void)physical_offset; (void)size; return 1; }
int xbox_PhysicalAddressResolve(uint32_t physical, uint32_t bytes, uint32_t *guest_va)
{ (void)bytes; *guest_va = physical; return 1; }
void xbox_Nv2aSemaphoreRelease(uint32_t dma_context, uint32_t semaphore_offset, uint32_t value)
{ (void)dma_context; (void)semaphore_offset; (void)value; }
void xbox_Nv2aNop(uint32_t param) { (void)param; }
