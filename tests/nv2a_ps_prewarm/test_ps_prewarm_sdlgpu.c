/* SDL3 GPU counterpart of the pixel-shader warm-up suite. The SDL renderer has
 * no specialised pixel shaders by design: its shaders are compiled ahead of
 * time (DXC), and a draw's dynamic state (alpha test, factors, constants)
 * travels in per-draw uniforms. So the warm-up switches must change nothing:
 * no manifest or sidecar is written under the cache directory, and draws whose
 * dynamic state changes between kicks render exact pixels. */
#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/video/nv2a_sdlgpu.c"
#include "../nv2a_sdlgpu_test_host.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

#define CACHE_DIR "ps_prewarm_sdl_cache"

static SDL_EnumerationResult count_entry(void *userdata, const char *dir, const char *name)
{
    (void)dir; (void)name;
    (*(unsigned *)userdata)++;
    return SDL_ENUM_CONTINUE;
}

/* Entries in the cache directory (SDL never lists . and ..). */
static unsigned cache_entries(void)
{
    unsigned count = 0;
    CHECK(SDL_EnumerateDirectory(CACHE_DIR, count_entry, &count));
    return count;
}

static int test_no_specialisation(void)
{
    static uint8_t ram[0x100000];
    NvD3DState st = {0};
    uint32_t black = 0xFF000000u, green = 0xFF00FF00u, red = 0xFFFF0000u, blue = 0xFF0000FFu;
    uint32_t pixel;
    nv2a_test_ram = ram;
    CHECK(SDL_CreateDirectory(CACHE_DIR));
    CHECK(cache_entries() == 0);
    st.color_addr = 0x10000; st.color_pitch = 64 * 4;
    st.surface_format = 0x28; st.clip_w = st.clip_h = 64;
    st.color_mask = 0x01010101;
    nv2a_d3d_clear(&st, 0xF0, black, 0, 0, 0, 0xFFFF, 0xFFFF);
    nv2a_test_triangle(&st, green, 0);
    pixel = nv2a_test_target_pixel(st.color_addr, 32, 32);
    CHECK(pixel == green);
    /* The alpha test's reference changes between draws with no new shader.
     * Diffuse alpha 0x80 passes GREATER refs 0x20 and 0x40 and fails 0xC0.
     * RGB only, so the opaque alpha of the clear is what remains. */
    st.color_mask = 0x00010101;
    st.alpha_test = 1; st.alpha_func = 0x204;
    st.alpha_ref = 0x40;
    nv2a_test_triangle(&st, 0x80FF0000u, 0);
    CHECK(nv2a_test_target_pixel(st.color_addr, 32, 32) == red);
    st.alpha_ref = 0xC0;
    nv2a_test_triangle(&st, 0x800000FFu, 0);
    CHECK(nv2a_test_target_pixel(st.color_addr, 32, 32) == red);
    st.alpha_ref = 0x20;
    nv2a_test_triangle(&st, 0x800000FFu, 0);
    CHECK(nv2a_test_target_pixel(st.color_addr, 32, 32) == blue);
    /* Specialisation switches on, cache directory set: still nothing written. */
    CHECK(cache_entries() == 0);
    CHECK(SDL_RemovePath(CACHE_DIR));
    puts("PASS: warm-up settings are inert (no manifest or sidecar written); dynamic alpha reference changes render exact pixels");
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    /* The D3D11 switches, set as a title would: the SDL renderer must ignore them. */
    nv2a_test_setenv("RECOMP_PS_SPEC", "1");
    nv2a_test_setenv("RECOMP_PS_PREWARM", "1");
    nv2a_test_setenv("RECOMP_SHADER_CACHE", CACHE_DIR);
    CHECK(nv2a_test_init());
    if (!strcmp(argv[1], "no_specialisation")) return test_no_specialisation();
    fprintf(stderr, "unknown case %s\n", argv[1]);
    return 1;
}
