/* The scanout AvSetDisplayMode records is what the renderer sizes its display
 * surface from; this drives the real AvSetDisplayMode and reads it back. */
#include "kernel.h"
#include <stdio.h>
/* This test must never dispatch guest code. */
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* Mode word as AvSendTVEncoderOption builds it: HDTV pack, standard in bits 8-15, refresh. */
#define MODE_NTSC_60 (AV_PACK_HDTV | (AV_STANDARD_NTSC_M << AV_STANDARD_SHIFT) | AV_REFRESH_60Hz)
#define MODE_PAL_50  (AV_PACK_HDTV | (AV_STANDARD_PAL_I << AV_STANDARD_SHIFT) | AV_REFRESH_50Hz)

int main(void) {
    uint32_t pitch = 0, bpp = 0, w = 0, h = 0;
    CHECK(!xbox_GetDisplayMode(&pitch, &bpp, &w, &h));

    /* 1024 pixels of X8R8G8B8: 4096-byte rows. The size is not a default 640. */
    xbox_AvSetDisplayMode(NULL, 0, MODE_NTSC_60, 0x1E, 4096, 0x03000000);
    CHECK(xbox_GetDisplayMode(&pitch, &bpp, &w, &h));
    CHECK(pitch == 4096 && bpp == 4 && w == 1024 && h == 480);

    /* PAL, 16-bit R5G6B5, 1280-byte rows: 640 x 576. */
    xbox_AvSetDisplayMode(NULL, 0, MODE_PAL_50, 0x11, 1280, 0x03000000);
    CHECK(xbox_GetDisplayMode(&pitch, &bpp, &w, &h));
    CHECK(pitch == 1280 && bpp == 2 && w == 640 && h == 576);

    /* A format this table does not decode is refused, not guessed at. */
    xbox_AvSetDisplayMode(NULL, 0, MODE_NTSC_60, 0x06, 2560, 0x03000000);
    CHECK(!xbox_GetDisplayMode(&pitch, &bpp, &w, &h));

    /* So is a mode with no standard. */
    xbox_AvSetDisplayMode(NULL, 0, AV_PACK_HDTV, 0x1E, 2560, 0x03000000);
    CHECK(!xbox_GetDisplayMode(&pitch, &bpp, &w, &h));

    puts("PASS: AvSetDisplayMode records pitch, format and size; undecodable modes are refused.");
    return 0;
}
