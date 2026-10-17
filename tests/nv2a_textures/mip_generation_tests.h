/* mip_reference and test_mip_inplace: texture_cpu_tests.h. */
static int check_mip_resource(ID3D11ShaderResourceView *view, const NvD3DTexture *texture,
                               const TexLayout *layout)
{
    ID3D11Resource *resource = NULL;
    ID3D11Texture2D *source = NULL, *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    uint32_t pixels[32 * 32], reduced[32 * 32];
    unsigned face, mip, row, w, h, levels = host_levels(layout, texture);
    ID3D11ShaderResourceView_GetResource(view, &resource);
    CHECK(SUCCEEDED(ID3D11Resource_QueryInterface(resource, &IID_ID3D11Texture2D, (void **)&source)));
    ID3D11Resource_Release(resource);
    ID3D11Texture2D_GetDesc(source, &desc);
    CHECK(desc.MipLevels == levels && desc.ArraySize == (UINT)layout->faces);
    CHECK(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &desc, NULL, &staging)));
    ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)staging, (ID3D11Resource *)source);
    for (face = 0; face < desc.ArraySize; face++) {
        w = texture->width; h = texture->height;
        for (mip = 0; mip < levels; mip++) {
            unsigned subresource = mip + face * levels;
            if (mip < (unsigned)layout->levels)
                decode_level(texture, layout, guest(texture->addr) + face * layout->face_stride +
                             layout->level_off[mip], mip, NULL, pixels);
            else {
                unsigned previous_w = texture->width >> (mip - 1);
                unsigned previous_h = texture->height >> (mip - 1);
                mip_reference(pixels, previous_w ? previous_w : 1,
                               previous_h ? previous_h : 1, reduced);
                memcpy(pixels, reduced, w * h * 4);
            }
            CHECK(SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)staging, subresource,
                                                   D3D11_MAP_READ, 0, &mapped)));
            for (row = 0; row < h; row++)
                CHECK(!memcmp((uint8_t *)mapped.pData + row * mapped.RowPitch, pixels + row * w, w * 4));
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)staging, subresource);
            if (w > 1) w /= 2;
            if (h > 1) h /= 2;
        }
    }
    ID3D11Texture2D_Release(staging); ID3D11Texture2D_Release(source);
    return 0;
}

static int test_mip_chain(void)
{
    const unsigned formats[] = {0x06, 0x07, 0x0C, 0x0E, 0x0F};
    const unsigned dimensions[][2] = {{16, 16}, {32, 8}, {1, 32}, {32, 1}};
    uint32_t random = 0xDAC35691u;
    unsigned format, dimension, iteration, i, count = 0;
    for (format = 0; format < sizeof formats / sizeof formats[0]; format++)
        for (dimension = 0; dimension < sizeof dimensions / sizeof dimensions[0]; dimension++) {
            NvD3DTexture texture = {0};
            TexLayout layout;
            ID3D11ShaderResourceView *first = NULL, *view;
            texture.addr = 0x20000 + count++ * 0x4000;
            texture.color = formats[format];
            texture.format = (formats[format] << 8) | 0x03220000u;
            texture.width = dimensions[dimension][0]; texture.height = dimensions[dimension][1];
            texture.levels = 2; texture.cube = dimension == 0;
            CHECK(tex_layout(&texture, &layout));
            for (iteration = 0; iteration < 2; iteration++) {
                size_t allocations;
                for (i = 0; i < layout.total; i++) ram[texture.addr + i] = (uint8_t)random_word(&random);
                nv2a_d3d_kick();
                allocations = pixel_allocations;
                view = texture_get(&texture);
                CHECK(view);
                if (iteration) {
                    CHECK(view == first);
                    CHECK(pixel_allocations == allocations);
                } else first = view;
                CHECK(!check_mip_resource(view, &texture, &layout));
            }
        }
    puts("PASS: short 2D/cube mip chains and changed uploads retain every GPU pixel without temporary allocation");
    return 0;
}

typedef void (*MipChainFunction)(uint32_t *, unsigned, unsigned);
static volatile uint32_t mip_sink;

/* Allocation/copy behavior before this change, using the same box filter. */
static void mip_chain_before(uint32_t *pixels, unsigned w, unsigned h)
{
    uint32_t *a = malloc((size_t)w * h * 4), *b = malloc((size_t)w * h * 4);
    memcpy(a, pixels, (size_t)w * h * 4);
    while (w > 1 || h > 1) {
        uint32_t *tmp;
        halve_bgra(a, w, h, b);
        if (w > 1) w /= 2;
        if (h > 1) h /= 2;
        mip_sink ^= b[w * h - 1];
        tmp = a; a = b; b = tmp;
    }
    free(a); free(b);
}

static void mip_chain_inplace(uint32_t *pixels, unsigned w, unsigned h)
{
    while (w > 1 || h > 1) {
        halve_bgra(pixels, w, h, pixels);
        if (w > 1) w /= 2;
        if (h > 1) h /= 2;
        mip_sink ^= pixels[w * h - 1];
    }
}

static __declspec(noinline) double time_mip_chain(MipChainFunction function,
    uint32_t *pixels, unsigned w, unsigned h, unsigned repetitions)
{
    LARGE_INTEGER frequency;
    int64_t start;
    unsigned iteration;
    QueryPerformanceFrequency(&frequency);
    start = qpc();
    for (iteration = 0; iteration < repetitions; iteration++) {
        pixels[0]++;
        function(pixels, w, h);
    }
    return (qpc() - start) * 1000.0 / frequency.QuadPart / repetitions;
}

static void sort_mip_times(double values[7])
{
    unsigned i, j;
    for (i = 0; i < 7; i++)
        for (j = i + 1; j < 7; j++)
            if (values[j] < values[i]) {
                double tmp = values[i]; values[i] = values[j]; values[j] = tmp;
            }
}

static int benchmark_mip_cpu(void)
{
    uint32_t *pixels = malloc(512 * 512 * 4);
    const unsigned sizes[] = {4, 16, 128, 512};
    unsigned size, sample;
    CHECK(pixels);
    memset(pixels, 127, 512 * 512 * 4);
    for (size = 0; size < sizeof sizes / sizeof sizes[0]; size++) {
        double before[7], after[7];
        unsigned repetitions = sizes[size] <= 16 ? 20000 : 200;
        for (sample = 0; sample < 7; sample++) {
            if (sample & 1) {
                after[sample] = time_mip_chain(mip_chain_inplace, pixels, sizes[size], sizes[size], repetitions);
                before[sample] = time_mip_chain(mip_chain_before, pixels, sizes[size], sizes[size], repetitions);
            } else {
                before[sample] = time_mip_chain(mip_chain_before, pixels, sizes[size], sizes[size], repetitions);
                after[sample] = time_mip_chain(mip_chain_inplace, pixels, sizes[size], sizes[size], repetitions);
            }
        }
        sort_mip_times(before); sort_mip_times(after);
        printf("MIP CPU %ux%u median: before %.6f ms, in-place %.6f ms, %.2fx\n",
               sizes[size], sizes[size], before[3], after[3], before[3] / after[3]);
    }
    free(pixels);
    return 0;
}

static int benchmark_mip_upload(void)
{
    NvD3DTexture texture = {0};
    TexLayout layout;
    LARGE_INTEGER frequency;
    double times[7];
    unsigned sample, iteration, i;
    size_t allocations = pixel_allocations;
    uint32_t random = 0xA2B4581Du;
    texture.addr = 0x100000; texture.color = 0x06; texture.format = 0x0AA40600u;
    texture.width = texture.height = 1024; texture.levels = 4;
    CHECK(tex_layout(&texture, &layout));
    for (i = 0; i < layout.total; i++) ram[texture.addr + i] = (uint8_t)random_word(&random);
    CHECK(texture_get(&texture));
    QueryPerformanceFrequency(&frequency);
    allocations = pixel_allocations;
    for (sample = 0; sample < 7; sample++) {
        int64_t start = qpc();
        for (iteration = 0; iteration < 100; iteration++) {
            ram[texture.addr]++;
            nv2a_d3d_kick();
            CHECK(texture_get(&texture));
        }
        ID3D11DeviceContext_Flush(s_ctx);
        times[sample] = (qpc() - start) * 1000.0 / frequency.QuadPart / 100;
    }
    sort_mip_times(times);
    printf("MIP UPLOAD 1024x1024, 4 guest/11 host levels: %.6f ms median per update, %zu temporary allocation attempts / 700 updates\n",
           times[3], pixel_allocations - allocations);
    return 0;
}
