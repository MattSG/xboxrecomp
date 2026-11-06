/* Actual presenter, with only the register snapshot and host window mocked. */
#include "video/fb_window.h"
#include "nv2a/nv2a_regs.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
static uint32_t regs[0x1000 / 4];
static ptrdiff_t offset;
static unsigned consumed;
size_t g_xbox_total_ram;
ptrdiff_t xbox_GetMemoryOffset(void) { return offset; }
void *fbw_native_window(void) { return NULL; }
float nv2a_d3d_display_aspect(void) { return 4.0f / 3.0f; }
bool nv2a_hook_pvideo_snapshot(uint32_t *out) { memcpy(out, regs, sizeof regs); return true; }
void nv2a_hook_pvideo_consume(unsigned bank) { consumed++; regs[NV_PVIDEO_BUFFER / 4] &= ~(1u << (bank * 4)); }
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
int main(void)
{
    uint8_t *pixels = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    XboxOverlay overlay;
    CHECK(pixels != NULL);
    if (!pixels) return 1;
    memset(pixels, 0x80, 4096);
    regs[NV_PVIDEO_FORMAT / 4] = (1u << 16) | 4u;
    regs[NV_PVIDEO_SIZE_IN / 4] = (2u << 16) | 2u;
    regs[NV_PVIDEO_SIZE_OUT / 4] = (2u << 16) | 2u;
    g_xbox_total_ram = 128u * 1024u * 1024u;
    /* Exact installed-RAM end must be admitted, even above 64 MB. */
    regs[NV_PVIDEO_BASE / 4] = (uint32_t)g_xbox_total_ram - 8;
    offset = (ptrdiff_t)((uintptr_t)pixels - (0x80000000u + regs[NV_PVIDEO_BASE / 4]));
    regs[NV_PVIDEO_BUFFER / 4] = 1;
    CHECK(xbox_FramebufferOverlay(&overlay));
    CHECK(consumed == 1 && overlay.pitch == 4 && !memcmp(overlay.yuy2, pixels, 8));
    regs[NV_PVIDEO_STOP / 4] = 1;
    CHECK(!xbox_FramebufferOverlay(&overlay));
    regs[NV_PVIDEO_STOP / 4] = 0;
    /* The same mapped pixels must not bypass the installed-RAM budget. */
    g_xbox_total_ram = 64u * 1024u * 1024u;
    regs[NV_PVIDEO_BUFFER / 4] = 1;
    CHECK(!xbox_FramebufferOverlay(&overlay));
    CHECK(consumed == 1);
    g_xbox_total_ram = 128u * 1024u * 1024u;
    regs[NV_PVIDEO_BASE / 4] += 1;
    CHECK(!xbox_FramebufferOverlay(&overlay));
    CHECK(consumed == 1);
    VirtualFree(pixels, 0, MEM_RELEASE);
    return failures != 0;
}
