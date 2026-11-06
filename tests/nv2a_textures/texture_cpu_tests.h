/* CPU texture decoding and in-place mip generation. These run the shared
 * nv2a_render_common.h code only, so both renderers' test builds include them
 * and every host runs them. */
#ifndef CHECK
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
#endif

static uint32_t random_word(uint32_t *state)
{
    *state ^= *state << 13; *state ^= *state >> 17; *state ^= *state << 5;
    return *state;
}

/* In-place generation must preserve every channel, mip, and cube face. */
static void mip_reference(const uint32_t *src, unsigned w, unsigned h, uint32_t *dst)
{
    unsigned nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
    unsigned x, y, c, dx, dy;
    for (y = 0; y < nh; y++)
        for (x = 0; x < nw; x++) {
            uint32_t pixel = 0;
            for (c = 0; c < 4; c++) {
                unsigned sum = 0;
                for (dy = 0; dy < 2; dy++)
                    for (dx = 0; dx < 2; dx++) {
                        unsigned sx = w > 1 ? x * 2 + dx : 0;
                        unsigned sy = h > 1 ? y * 2 + dy : 0;
                        sum += (src[sy * w + sx] >> (8 * c)) & 255;
                    }
                pixel |= ((sum + 2) / 4) << (8 * c);
            }
            dst[y * nw + x] = pixel;
        }
}

static int test_mip_inplace(void)
{
    const unsigned dimensions[] = {1, 2, 3, 4, 5, 7, 8, 15, 16, 31, 64, 128};
    uint32_t original[128 * 128], reference[128 * 128], actual[128 * 128 + 2];
    uint32_t random = 0x58A42F01u;
    unsigned wi, hi, i, w, h, nw, nh;
    for (wi = 0; wi < sizeof dimensions / sizeof dimensions[0]; wi++)
        for (hi = 0; hi < sizeof dimensions / sizeof dimensions[0]; hi++) {
            w = dimensions[wi]; h = dimensions[hi];
            for (i = 0; i < w * h; i++) original[i] = random_word(&random);
            memcpy(actual + 1, original, w * h * 4);
            actual[0] = actual[1 + w * h] = 0xA5E37C91u;
            while (w > 1 || h > 1) {
                nw = w > 1 ? w / 2 : 1; nh = h > 1 ? h / 2 : 1;
                mip_reference(original, w, h, reference);
                halve_bgra(actual + 1, w, h, actual + 1);
                CHECK(!memcmp(reference, actual + 1, nw * nh * 4));
                CHECK(actual[0] == 0xA5E37C91u);
                CHECK(actual[1 + dimensions[wi] * dimensions[hi]] == 0xA5E37C91u);
                memcpy(original, reference, nw * nh * 4);
                w = nw; h = nh;
            }
        }
    puts("PASS: in-place mip chains exactly match independent channel averaging for 144 rectangles");
    return 0;
}

static int test_decode_equivalence(void)
{
    uint32_t state = 0x12345678u;
    uint8_t *data = malloc(65536 + 64);
    uint32_t *allocation = malloc(65536 + 16), *decoded;
    const unsigned formats[] = { 0x06, 0x07, 0x0C, 0x0E, 0x0F };
    const unsigned dimensions[] = { 1, 2, 3, 4, 5, 7, 8, 12, 16, 20, 64, 128 };
    unsigned i, offset, format, width, height, w, h, x, y;
    CHECK(data);
    CHECK(allocation);
    decoded = allocation + 1;
    for (i = 0; i < 65536 + 64; i++) data[i] = (uint8_t)random_word(&state);
    /* Validate complete rectangles, unaligned sources, short mips and both
     * compressed alpha modes against independent per-texel addressing. */
    for (format = 0; format < 5; format++)
        for (width = 0; width < sizeof dimensions / sizeof dimensions[0]; width++)
            for (height = 0; height < sizeof dimensions / sizeof dimensions[0]; height++)
                for (offset = 0; offset < 8; offset++) {
                    NvD3DTexture texture = {0};
                    TexLayout layout;
                    w = dimensions[width]; h = dimensions[height];
                    if (format < 2 && ((w & (w - 1)) || (h & (h - 1)))) continue;
                    texture.color = formats[format];
                    texture.width = w; texture.height = h; texture.levels = 1;
                    CHECK(tex_layout(&texture, &layout));
                    decoded[-1] = decoded[w * h] = 0xDEADC0DEu;
                    decode_level(&texture, &layout, data + offset, 0, NULL, decoded);
                    CHECK(decoded[-1] == 0xDEADC0DEu && decoded[w * h] == 0xDEADC0DEu);
                    for (y = 0; y < h; y++)
                        for (x = 0; x < w; x++) {
                            uint32_t expected;
                            if (layout.kind == K_DXT)
                                CHECK(d3d8_dxt_decode_texel(data + offset, texture.color,
                                                           x, y, w, &expected));
                            else {
                                uint32_t address = swizzle_offset(x, y, w, h);
                                expected = texel(texture.color, data + offset + address * 4, NULL, 0);
                            }
                            CHECK(decoded[y * w + x] == expected);
                        }
        }
    free(data);
    free(allocation);
    puts("PASS: tiled32-bit and DXT uploads match per-texel decoding across sizes, mips and alignments");
    return 0;
}
