/* SDL3 GPU texture cache, the counterpart of test_textures.c: failed resource
 * creation retries without a poisoned entry, complete mip publication, a
 * scratch allocation failure keeps the previous image, palette length is part
 * of identity, and generated mips match the reference. Pixels are read back
 * from the GPU. Shared CPU cases run in nv2a_textures_cpu. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Failure injection in the renderer's own calls: texture creation and the
 * scratch allocation that holds generated mips. */
static int fail_create;
static uint32_t created_levels;
static SDL_GPUTextureFormat created_format;
static size_t fail_alloc_size;
static int fail_alloc;

static SDL_GPUTexture *test_create_texture(SDL_GPUDevice *device, const SDL_GPUTextureCreateInfo *info)
{
    if (fail_create) {
        fail_create--;
        return NULL;
    }
    created_levels=info->num_levels; created_format=info->format;
    return (SDL_CreateGPUTexture)(device, info);
}

static void *test_alloc(size_t bytes)
{
    if (fail_alloc && bytes == fail_alloc_size) {
        fail_alloc--;
        return NULL;
    }
    return (malloc)(bytes);
}

#define SDL_CreateGPUTexture(device, info) test_create_texture(device, info)
#define malloc(bytes) test_alloc(bytes)
#include "../../src/video/nv2a_sdlgpu.c"
#include "../nv2a_sdlgpu_test_host.h"
#include "texture_cpu_tests.h"
#undef malloc

/* Texel (0, 0) of one mip of one face; the whole level is read back. */
static int read_texel0(SDL_GPUTexture *view, uint32_t w, uint32_t h, uint32_t mip, uint32_t *pixel)
{
    uint32_t *level = malloc((size_t)w * h * 4);
    int ok;
    if (!level) return 0;
    ok = nv2a_test_read_level(view, w, h, mip, 0, level);
    *pixel = level[0];
    free(level);
    return ok;
}

static int test_srv_retry(void)
{
    NvD3DTexture texture = {0};
    SDL_GPUTexture *view;
    size_t count = s_tex_count, bytes = s_tex_bytes;
    uint32_t i, pixel = 0;
    texture.addr = 0x20000;
    texture.color = 0x12;
    texture.format = 0x00011200;
    texture.width = texture.height = 4;
    texture.pitch = 16; texture.levels = 1;
    for (i = 0; i < 16; i++) ((uint32_t *)(nv2a_test_ram + texture.addr))[i] = 0xFF00FF00u;
    fail_create = 1;
    CHECK(texture_get_with_levels(&texture, NULL) == NULL);
    CHECK(fail_create == 0);
    CHECK(s_tex_count == count && s_tex_bytes == bytes);
    /* Retry in the same kick: the failed texture must never become a cache hit. */
    view = texture_get_with_levels(&texture, NULL);
    CHECK(view && read_texel0(view, 4, 4, 0, &pixel) && pixel == 0xFF00FF00u);
    CHECK(texture_get_with_levels(&texture, NULL) == view);
    nv2a_d3d_kick();
    CHECK(texture_get_with_levels(&texture, NULL) == view);
    CHECK(s_tex_count == count + 1);
    CHECK(s_tex_bytes == bytes + 16 * 4);
    puts("PASS: failed texture creation retries immediately without a poisoned cache entry");
    return 0;
}

static int test_mip_retry(void)
{
    NvD3DTexture texture = {0};
    SDL_GPUTexture *view;
    size_t count = s_tex_count, bytes = s_tex_bytes;
    uint32_t i, pixel = 0;
    texture.addr = 0x30000;
    texture.color = 0x06;
    texture.format = 0x03320600;
    texture.width = texture.height = 8; texture.levels = 2;
    for (i = 0; i < 64; i++) ((uint32_t *)(nv2a_test_ram + texture.addr))[i] = 0xFFFF0000u;
    for (i = 64; i < 80; i++) ((uint32_t *)(nv2a_test_ram + texture.addr))[i] = 0xFF0000FFu;
    /* Generated mips reuse the top-level scratch. An allocation failure must
     * still leave the complete previous GPU image visible and retryable. */
    free(s_up_pixels); s_up_pixels = NULL; s_up_cap = 0;
    fail_alloc_size = 8 * 8 * 4;
    fail_alloc = 1;
    CHECK(texture_get_with_levels(&texture, NULL) == NULL);
    CHECK(fail_alloc == 0);
    CHECK(s_tex_count == count && s_tex_bytes == bytes);
    view = texture_get_with_levels(&texture, NULL);
    CHECK(view);
    CHECK(read_texel0(view, 8, 8, 0, &pixel) && pixel == 0xFFFF0000u);
    for (i = 1; i < 4; i++) {
        uint32_t size = 8u >> i ? 8u >> i : 1;
        CHECK(read_texel0(view, size, size, i, &pixel) && pixel == 0xFF0000FFu);
    }
    CHECK(s_tex_count == count + 1);
    CHECK(s_tex_bytes == bytes + (64 + 16 + 4 + 1) * 4);
    /* Also retry a failed update of an existing resource. */
    for (i = 64; i < 80; i++) ((uint32_t *)(nv2a_test_ram + texture.addr))[i] = 0xFF00FF00u;
    nv2a_d3d_kick();
    free(s_up_pixels); s_up_pixels = NULL; s_up_cap = 0;
    fail_alloc = 1;
    CHECK(texture_get_with_levels(&texture, NULL) == view);
    CHECK(fail_alloc == 0 && s_tex_count == count + 1);
    for (i = 1; i < 4; i++) {
        uint32_t size = 8u >> i ? 8u >> i : 1;
        CHECK(read_texel0(view, size, size, i, &pixel) && pixel == 0xFF0000FFu);
    }
    CHECK(texture_get_with_levels(&texture, NULL) == view);
    for (i = 1; i < 4; i++) {
        uint32_t size = 8u >> i ? 8u >> i : 1;
        CHECK(read_texel0(view, size, size, i, &pixel) && pixel == 0xFF00FF00u);
    }
    puts("PASS: scratch allocation failure cannot publish incomplete mips and retries existing uploads");
    return 0;
}

static int test_palette_identity(void)
{
    NvD3DTexture texture = {0};
    SDL_GPUTexture *short_view, *long_view;
    uint32_t pixel = 0;
    texture.addr = 0x40000;
    texture.color = 0x0B; texture.format = 0x02210B00;
    texture.width = texture.height = 4; texture.levels = 1;
    texture.palette = 0x41000; texture.palette_len = 32;
    memset(nv2a_test_ram + texture.addr, 7, 16);
    ((uint32_t *)(nv2a_test_ram + texture.palette))[7] = 0xFFFF0000u;
    short_view = texture_get_with_levels(&texture, NULL);
    CHECK(short_view && read_texel0(short_view, 4, 4, 0, &pixel) && pixel == 0xFFFF0000u);
    /* PALETTE_LENGTH is a separate register from FORMAT and may change
     * between draws of a kick. Their validation/hash spans differ. */
    texture.palette_len = 256;
    long_view = texture_get_with_levels(&texture, NULL);
    CHECK(long_view && long_view != short_view);
    CHECK(read_texel0(long_view, 4, 4, 0, &pixel) && pixel == 0xFFFF0000u);
    texture.palette_len = 32;
    CHECK(texture_get_with_levels(&texture, NULL) == short_view);
    puts("PASS: independently changed palette length cannot bypass cache validation");
    return 0;
}

/* The generated image for one face and mip: the guest level decoded by the
 * shared CPU code, or a reduction of the previous level. */
static void expected_mip(const NvD3DTexture *texture, const TexLayout *layout, uint32_t face,
                         uint32_t mip, uint32_t *pixels)
{
    uint32_t w = texture->width >> mip ? texture->width >> mip : 1;
    uint32_t h = texture->height >> mip ? texture->height >> mip : 1;
    if (mip < (uint32_t)layout->levels) {
        decode_level(texture, layout, nv2a_test_ram + texture->addr + face * layout->face_stride +
                     layout->level_off[mip], (int)mip, NULL, pixels);
    } else {
        uint32_t pw = texture->width >> (mip - 1), ph = texture->height >> (mip - 1);
        uint32_t reduced[32 * 32];
        mip_reference(pixels, pw ? pw : 1, ph ? ph : 1, reduced);
        memcpy(pixels, reduced, (size_t)w * h * 4);
    }
}

static int check_mip_resource(SDL_GPUTexture *view, const NvD3DTexture *texture, const TexLayout *layout)
{
    uint32_t pixels[32 * 32], got[32 * 32];
    uint32_t face, mip, w, h, levels = host_levels(layout, texture);
    for (face = 0; face < (uint32_t)layout->faces; face++) {
        w = texture->width; h = texture->height;
        for (mip = 0; mip < levels; mip++) {
            expected_mip(texture, layout, face, mip, pixels);
            CHECK(nv2a_test_read_level(view, w, h, mip, face, got));
            CHECK(!memcmp(got, pixels, (size_t)w * h * 4));
            if (w > 1) w /= 2;
            if (h > 1) h /= 2;
        }
    }
    return 0;
}

static int test_mip_chain(void)
{
    /* Two guest levels never complete the chain, so every case here is a
     * BGRA host image with generated mips (the native BC path is a separate
     * case, native_bc). */
    const unsigned formats[] = {0x06, 0x07, 0x0C, 0x0E, 0x0F};
    const unsigned dimensions[][2] = {{16, 16}, {32, 8}, {1, 32}, {32, 1}};
    uint32_t random = 0xDAC35691u;
    unsigned format, dimension, iteration, i, count = 0, checked = 0;
    for (format = 0; format < sizeof formats / sizeof formats[0]; format++)
        for (dimension = 0; dimension < sizeof dimensions / sizeof dimensions[0]; dimension++) {
            NvD3DTexture texture = {0};
            TexLayout layout;
            SDL_GPUTexture *first = NULL, *view;
            texture.addr = 0x20000 + count++ * 0x4000;
            texture.color = formats[format];
            texture.format = (formats[format] << 8) | 0x03220000u;
            texture.width = dimensions[dimension][0]; texture.height = dimensions[dimension][1];
            texture.levels = 2; texture.cube = dimension == 0;
            CHECK(tex_layout(&texture, &layout));
            CHECK(texture_host_format(&texture, &layout, host_levels(&layout, &texture)) ==
                  SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
            checked++;
            for (iteration = 0; iteration < 2; iteration++) {
                for (i = 0; i < layout.total; i++) nv2a_test_ram[texture.addr + i] = (uint8_t)random_word(&random);
                nv2a_d3d_kick();
                view = texture_get_with_levels(&texture, NULL);
                CHECK(view);
                if (iteration) CHECK(view == first);
                else first = view;
                CHECK(!check_mip_resource(view, &texture, &layout));
            }
        }
    CHECK(checked == 20);
    printf("PASS: generated mip chains match the reference on the GPU for %u textures\n", checked);
    return 0;
}

static int test_guest_mips(void)
{
    NvD3DTexture t={0};
    SDL_GPUTexture *view;
    uint32_t pixels[64], pixel, i;
    size_t before=s_tex_bytes;
    t.addr=0x30000; t.color=0x06; t.format=0x03320600;
    t.width=t.height=8; t.levels=2;
    for(i=0;i<80;i++) ((uint32_t *)(nv2a_test_ram+t.addr))[i]=i<64?0xFFFF0000u:0xFF0000FFu;
    view=texture_get_with_levels(&t,NULL); CHECK(view);
    CHECK(created_levels==2 && created_format==SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
    CHECK(s_tex_bytes==before+320);
    CHECK(read_texel0(view,8,8,0,&pixel) && pixel==0xFFFF0000u);
    CHECK(read_texel0(view,4,4,1,&pixel) && pixel==0xFF0000FFu);
    /* A short BC1 chain remains compressed when completion is not selected. */
    t.addr=0x40000; t.color=0x0C; t.format=0x00020C00;
    for(i=0;i<40;i++) nv2a_test_ram[t.addr+i]=(uint8_t)(i*13u);
    view=texture_get_with_levels(&t,NULL); CHECK(view);
    CHECK(created_levels==2 && created_format==SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM);
    CHECK(s_tex_bytes==before+360);
    CHECK(nv2a_test_read_level(view,8,8,0,0,pixels));
    CHECK(!memcmp(pixels,nv2a_test_ram+t.addr,32));
    CHECK(nv2a_test_read_level(view,4,4,1,0,pixels));
    CHECK(!memcmp(pixels,nv2a_test_ram+t.addr+32,8));
    puts("PASS: default authored mip pixels, BC1 format and compressed bytes retained");
    return 0;
}

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    nv2a_test_ram = calloc(1, 0x100000);
    CHECK(nv2a_test_ram);
    CHECK(nv2a_test_init());
    if (!strcmp(argv[1], "guest_mips")) return test_guest_mips();
    if (!strcmp(argv[1], "srv_retry")) return test_srv_retry();
    if (!strcmp(argv[1], "mip_retry")) return test_mip_retry();
    if (!strcmp(argv[1], "palette_identity")) return test_palette_identity();
    if (!strcmp(argv[1], "mip_chain")) return test_mip_chain();
    fprintf(stderr, "unknown case %s\n", argv[1]);
    return 1;
}
