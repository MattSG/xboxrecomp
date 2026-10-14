/* Sample actual BC resources and the established decoded BGRA reference on
 * the GPU, including every mip/face and filtered taps between texel centers. */
#include <math.h>

static ID3D11ComputeShader *bc_compare_shader;
static ID3D11SamplerState *bc_compare_sampler[2];
static ID3D11Buffer *bc_compare_constants;
static float bc_max_rgb_error, bc_max_alpha_error;

static int bc_compare_init(void)
{
    static const char shader[] =
        "Texture2DArray<float4> compressed : register(t0);"
        "Texture2DArray<float4> decoded : register(t1);"
        "SamplerState taps : register(s0);"
        "struct Pair { float4 compressed; float4 decoded; };"
        "RWStructuredBuffer<Pair> output : register(u0);"
        "cbuffer Params : register(b0) { uint width,height,mip,face; float2 shift,pad; };"
        "[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {"
        " if(id.x>=width || id.y>=height) return;"
        " float3 uv=float3((float2(id.xy)+0.5+shift)/float2(width,height),face);"
        " Pair p; p.compressed=compressed.SampleLevel(taps,uv,mip);"
        " p.decoded=decoded.SampleLevel(taps,uv,mip); output[id.y*width+id.x]=p; }";
    ID3DBlob *code = NULL, *errors = NULL;
    D3D11_SAMPLER_DESC sampler_desc;
    unsigned i;
    HRESULT hr = D3DCompile(shader, sizeof shader - 1, "bc_compare", NULL, NULL,
                            "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) && errors)
        fprintf(stderr, "%s\n", (const char *)ID3D10Blob_GetBufferPointer(errors));
    if (errors) ID3D10Blob_Release(errors);
    CHECK(SUCCEEDED(hr) && code);
    hr = ID3D11Device_CreateComputeShader(s_dev, ID3D10Blob_GetBufferPointer(code),
                                         ID3D10Blob_GetBufferSize(code), NULL, &bc_compare_shader);
    ID3D10Blob_Release(code);
    CHECK(SUCCEEDED(hr));
    memset(&sampler_desc, 0, sizeof sampler_desc);
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    for (i = 0; i < 2; i++) {
        sampler_desc.Filter = i ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        CHECK(SUCCEEDED(ID3D11Device_CreateSamplerState(s_dev, &sampler_desc, &bc_compare_sampler[i])));
    }
    bc_compare_constants = make_buffer(32, D3D11_BIND_CONSTANT_BUFFER);
    CHECK(bc_compare_constants);
    return 0;
}

static ID3D11ShaderResourceView *bc_array_view(ID3D11ShaderResourceView *original,
                                             D3D11_TEXTURE2D_DESC *texture_desc)
{
    ID3D11Resource *resource = NULL;
    ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *array = NULL;
    D3D11_SHADER_RESOURCE_VIEW_DESC view_desc;
    ID3D11ShaderResourceView_GetResource(original, &resource);
    if (FAILED(ID3D11Resource_QueryInterface(resource, &IID_ID3D11Texture2D, (void **)&texture)))
        goto done;
    ID3D11Texture2D_GetDesc(texture, texture_desc);
    memset(&view_desc, 0, sizeof view_desc);
    view_desc.Format = texture_desc->Format;
    view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    view_desc.Texture2DArray.MipLevels = texture_desc->MipLevels;
    view_desc.Texture2DArray.ArraySize = texture_desc->ArraySize;
    ID3D11Device_CreateShaderResourceView(s_dev, resource, &view_desc, &array);
done:
    if (texture) ID3D11Texture2D_Release(texture);
    if (resource) ID3D11Resource_Release(resource);
    return array;
}

static ID3D11ShaderResourceView *bc_reference(const NvD3DTexture *t, const TexLayout *layout)
{
    D3D11_SUBRESOURCE_DATA data[16 * 6];
    uint32_t *pixels[16 * 6] = {0};
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D *texture = NULL;
    ID3D11ShaderResourceView *view = NULL;
    int face, mip, count = 0, levels = host_levels(layout, t);
    memset(data, 0, sizeof data);
    for (face = 0; face < layout->faces; face++)
        for (mip = 0; mip < levels; mip++) {
            uint32_t width = t->width >> mip ? t->width >> mip : 1;
            uint32_t height = t->height >> mip ? t->height >> mip : 1;
            uint32_t x, y;
            pixels[count] = malloc((size_t)width * height * 4);
            if (!pixels[count]) goto done;
            if (mip < layout->levels) {
                const uint8_t *src = guest(t->addr) + face * layout->face_stride + layout->level_off[mip];
                for (y = 0; y < height; y++)
                    for (x = 0; x < width; x++)
                        if (!d3d8_dxt_decode_texel(src, t->color, x, y, width, &pixels[count][y * width + x]))
                            goto done;
            } else {
                uint32_t previous_width = t->width >> (mip - 1) ? t->width >> (mip - 1) : 1;
                uint32_t previous_height = t->height >> (mip - 1) ? t->height >> (mip - 1) : 1;
                halve_bgra(pixels[count - 1], previous_width, previous_height, pixels[count]);
            }
            data[count].pSysMem = pixels[count];
            data[count].SysMemPitch = width * 4;
            count++;
        }
    memset(&desc, 0, sizeof desc);
    desc.Width = t->width; desc.Height = t->height;
    desc.MipLevels = levels; desc.ArraySize = layout->faces;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = t->cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev, &desc, data, &texture)))
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)texture, NULL, &view);
done:
    if (texture) ID3D11Texture2D_Release(texture);
    for (count = 0; count < 16 * 6; count++) free(pixels[count]);
    return view;
}

/* D3D11 spec 19.5.2 permits 1/255 + 3% of the endpoint span for BC1/2/3.
 * Add the exact difference between its promoted/interpolated reference and
 * our existing integer decoder, rather than using a guessed global epsilon.
 * https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#19.5.2 */
static float bc_texel_tolerance(const uint8_t *src, unsigned format, unsigned width,
                                 unsigned x, unsigned y, unsigned channel)
{
    unsigned block_bytes = d3d8_format_dxt_block_bytes(format);
    const uint8_t *block = src + ((size_t)(y / 4) * ((width + 3) / 4) + x / 4) * block_bytes;
    unsigned selector, texel = (y & 3) * 4 + (x & 3), expected;
    float ideal, span;
    d3d8_dxt_decode_texel(src, format, x, y, width, &expected);
    if (channel < 3) {
        const uint8_t *color = block + (block_bytes == 16 ? 8 : 0);
        unsigned c0 = color[0] | (color[1] << 8), c1 = color[2] | (color[3] << 8);
        unsigned bits = channel == 1 ? 6 : 5, shift = channel == 0 ? 11 : channel == 1 ? 5 : 0;
        unsigned a = (c0 >> shift) & ((1u << bits) - 1), b = (c1 >> shift) & ((1u << bits) - 1);
        unsigned ap = (a << (8 - bits)) | (a >> (2 * bits - 8));
        unsigned bp = (b << (8 - bits)) | (b >> (2 * bits - 8));
        unsigned indices = color[4] | (color[5] << 8) | (color[6] << 16) | ((unsigned)color[7] << 24);
        selector = (indices >> (2 * texel)) & 3;
        span = fmaxf(fabsf((float)ap - bp), fabsf((float)a - b) * 255 / ((1u << bits) - 1));
        ideal = selector == 0 ? (float)ap : selector == 1 ? (float)bp :
                (c0 > c1 || block_bytes == 16) ?
                    (selector == 2 ? (2.0f * ap + bp) / 3 : (ap + 2.0f * bp) / 3) :
                    selector == 2 ? (ap + bp) / 2.0f : 0;
        expected = (expected >> (16 - channel * 8)) & 255;
    } else {
        expected >>= 24;
        if (format != 0x0F) return 0.001f; /* BC1 one-bit and BC2 explicit alpha. */
        {
            uint64_t indices = 0;
            unsigned i, a = block[0], b = block[1];
            for (i = 0; i < 6; i++) indices |= (uint64_t)block[2 + i] << (8 * i);
            selector = (unsigned)((indices >> (3 * texel)) & 7);
            span = fabsf((float)a - b);
            ideal = selector == 0 ? (float)a : selector == 1 ? (float)b :
                    a > b ? ((8 - selector) * (float)a + (selector - 1) * (float)b) / 7 :
                    selector < 6 ? ((6 - selector) * (float)a + (selector - 1) * (float)b) / 5 :
                    selector == 6 ? 0 : 255;
        }
    }
    /* The specification requires exact extreme values, regardless of span. */
    if (ideal == 0 || ideal == 255) return 0.001f;
    return 1.0f + 0.03f * span + fabsf(ideal - expected) + 0.001f;
}

static float bc_sample_tolerance(const NvD3DTexture *t, const TexLayout *layout,
                                  const uint8_t *snapshot, unsigned face, unsigned mip,
                                  unsigned x, unsigned y, unsigned channel, unsigned filter)
{
    unsigned width = layout->level_w[mip], height = layout->level_h[mip];
    const uint8_t *src = snapshot + face * layout->face_stride + layout->level_off[mip];
    if (!filter) return bc_texel_tolerance(src, t->color, width, x, y, channel);
    /* The linear test's shifts are (+1/4,-1/4) texel, with clamp addressing. */
    {
        unsigned x1 = x + 1 < width ? x + 1 : x, y0 = y ? y - 1 : 0;
        return 0.1875f * bc_texel_tolerance(src, t->color, width, x, y0, channel) +
               0.0625f * bc_texel_tolerance(src, t->color, width, x1, y0, channel) +
               0.5625f * bc_texel_tolerance(src, t->color, width, x, y, channel) +
               0.1875f * bc_texel_tolerance(src, t->color, width, x1, y, channel);
    }
}

static int bc_compare(ID3D11ShaderResourceView *compressed, ID3D11ShaderResourceView *decoded,
                       int native, const NvD3DTexture *t, const TexLayout *layout,
                       const uint8_t *snapshot)
{
    D3D11_TEXTURE2D_DESC a_desc, b_desc;
    ID3D11ShaderResourceView *views[2] = { bc_array_view(compressed, &a_desc), bc_array_view(decoded, &b_desc) };
    D3D11_BUFFER_DESC buffer_desc;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc;
    ID3D11Buffer *output = NULL, *staging = NULL;
    ID3D11UnorderedAccessView *uav = NULL;
    D3D11_MAPPED_SUBRESOURCE mapped;
    unsigned face, mip, filter;
    CHECK(views[0] && views[1]);
    CHECK(a_desc.Width == b_desc.Width && a_desc.Height == b_desc.Height);
    CHECK(a_desc.MipLevels == b_desc.MipLevels && a_desc.ArraySize == b_desc.ArraySize);
    memset(&buffer_desc, 0, sizeof buffer_desc);
    buffer_desc.ByteWidth = a_desc.Width * a_desc.Height * 8 * sizeof(float);
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    buffer_desc.StructureByteStride = 8 * sizeof(float);
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(s_dev, &buffer_desc, NULL, &output)));
    memset(&uav_desc, 0, sizeof uav_desc);
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.NumElements = a_desc.Width * a_desc.Height;
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(s_dev, (ID3D11Resource *)output, &uav_desc, &uav)));
    buffer_desc.Usage = D3D11_USAGE_STAGING;
    buffer_desc.BindFlags = buffer_desc.MiscFlags = buffer_desc.StructureByteStride = 0;
    buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(s_dev, &buffer_desc, NULL, &staging)));
    for (face = 0; face < a_desc.ArraySize; face++)
        for (mip = 0; mip < a_desc.MipLevels; mip++)
            for (filter = 0; filter < 2; filter++) {
                struct { uint32_t width, height, mip, face; float shift[2], pad[2]; } params = {0};
                ID3D11UnorderedAccessView *null_uav = NULL;
                unsigned pixel, channel;
                params.width = a_desc.Width >> mip ? a_desc.Width >> mip : 1;
                params.height = a_desc.Height >> mip ? a_desc.Height >> mip : 1;
                params.mip = mip; params.face = face;
                params.shift[0] = filter ? 0.25f : 0;
                params.shift[1] = filter ? -0.25f : 0;
                upload_cb(bc_compare_constants, &params, sizeof params);
                ID3D11DeviceContext_CSSetShader(s_ctx, bc_compare_shader, NULL, 0);
                ID3D11DeviceContext_CSSetShaderResources(s_ctx, 0, 2, views);
                ID3D11DeviceContext_CSSetSamplers(s_ctx, 0, 1, &bc_compare_sampler[filter]);
                ID3D11DeviceContext_CSSetConstantBuffers(s_ctx, 0, 1, &bc_compare_constants);
                ID3D11DeviceContext_CSSetUnorderedAccessViews(s_ctx, 0, 1, &uav, NULL);
                ID3D11DeviceContext_Dispatch(s_ctx, (params.width + 7) / 8, (params.height + 7) / 8, 1);
                ID3D11DeviceContext_CSSetUnorderedAccessViews(s_ctx, 0, 1, &null_uav, NULL);
                ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)staging, (ID3D11Resource *)output);
                CHECK(SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped)));
                for (pixel = 0; pixel < params.width * params.height; pixel++)
                    for (channel = 0; channel < 4; channel++) {
                        const float *pair = (const float *)mapped.pData + pixel * 8;
                        float error = fabsf(pair[channel] - pair[4 + channel]) * 255.0f;
                        float tolerance = native ? bc_sample_tolerance(t, layout, snapshot, face, mip,
                                pixel % params.width, pixel / params.width, channel, filter) : 0.001f;
                        if (channel == 3) {
                            if (error > bc_max_alpha_error) bc_max_alpha_error = error;
                        } else if (error > bc_max_rgb_error) bc_max_rgb_error = error;
                        if (error > tolerance) {
                            fprintf(stderr, "BC mismatch format=%u face=%u mip=%u filter=%u pixel=%u channel=%u compressed=%.5f decoded=%.5f error=%.5f tolerance=%.5f\n",
                                    a_desc.Format, face, mip, filter, pixel, channel,
                                    pair[channel] * 255, pair[4 + channel] * 255, error, tolerance);
                            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)staging, 0);
                            CHECK(0);
                        }
                    }
                ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)staging, 0);
            }
    {
        ID3D11ShaderResourceView *none[2] = {NULL, NULL};
        ID3D11DeviceContext_CSSetShaderResources(s_ctx, 0, 2, none);
        ID3D11DeviceContext_CSSetShader(s_ctx, NULL, NULL, 0);
    }
    ID3D11ShaderResourceView_Release(views[0]); ID3D11ShaderResourceView_Release(views[1]);
    ID3D11UnorderedAccessView_Release(uav);
    ID3D11Buffer_Release(output); ID3D11Buffer_Release(staging);
    return 0;
}

static int test_native_bc(void)
{
    static const unsigned formats[] = {0x0C, 0x0E, 0x0F};
    static const DXGI_FORMAT host_formats[] = {DXGI_FORMAT_BC1_UNORM, DXGI_FORMAT_BC2_UNORM, DXGI_FORMAT_BC3_UNORM};
    static const struct { unsigned width, height, levels, cube, native; } cases[] = {
        {8,8,4,0,1}, {16,8,5,0,1}, {12,20,5,0,1}, {4,4,3,1,1}, {8,8,4,1,1},
        {8,8,2,0,0}, {8,8,2,1,0}, {3,8,4,0,0}, {2,2,2,0,0}, {4,4,1,0,1}
    };
    static const unsigned alignments[] = {0,1,3,7};
    uint32_t state = 0x9A6F031Bu;
    unsigned format, item, alignment, sequence = 0;
    CHECK(bc_compare_init() == 0);
    for (format = 0; format < 3; format++)
        for (item = 0; item < sizeof cases / sizeof cases[0]; item++)
            for (alignment = 0; alignment < sizeof alignments / sizeof alignments[0]; alignment++) {
                NvD3DTexture texture = {0};
                TexLayout layout;
                ID3D11ShaderResourceView *view, *reference;
                uint8_t *snapshot;
                D3D11_SHADER_RESOURCE_VIEW_DESC view_desc;
                D3D11_TEXTURE2D_DESC texture_desc;
                ID3D11ShaderResourceView *array;
                size_t count = s_tex_count, bytes = s_tex_bytes, expected_bytes = 0;
                unsigned i, mip, width, height, levels;
                texture.addr = 0x50000 + sequence++ * 4096 + alignments[alignment];
                texture.color = formats[format];
                texture.width = cases[item].width; texture.height = cases[item].height;
                texture.levels = cases[item].levels; texture.cube = cases[item].cube;
                texture.format = (texture.color << 8) | (texture.levels << 16);
                CHECK(tex_layout(&texture, &layout));
                CHECK(guest_ok(texture.addr, layout.total));
                for (i = 0; i < layout.total; i++) ram[texture.addr + i] = (uint8_t)random_word(&state);
                /* Guarantee exact extrema and BC1 transparent selector 3 in
                 * every face/mip, rather than relying on randomized endpoints. */
                {
                    unsigned face, level;
                    for (face = 0; face < (unsigned)layout.faces; face++)
                        for (level = 0; level < (unsigned)layout.levels; level++) {
                            uint8_t *block = ram + texture.addr + face * layout.face_stride + layout.level_off[level];
                            uint8_t *color = block + (layout.bpp == 16 ? 8 : 0);
                            color[0] = color[1] = 0; color[2] = color[3] = 255;
                            color[4] = 0xE7; color[5] = color[6] = color[7] = 0xE4;
                            if (texture.color == 0x0E) memset(block, 0xF0, 8);
                            if (texture.color == 0x0F) {
                                static const unsigned selectors[] = {0,1,6,7};
                                uint64_t indices = 0;
                                unsigned pixel;
                                block[0] = 0; block[1] = 255;
                                for (pixel = 0; pixel < 16; pixel++) indices |= (uint64_t)selectors[pixel & 3] << (3 * pixel);
                                for (pixel = 0; pixel < 6; pixel++) block[2 + pixel] = (uint8_t)(indices >> (8 * pixel));
                            }
                        }
                }
                snapshot = malloc(layout.total);
                CHECK(snapshot);
                memcpy(snapshot, ram + texture.addr, layout.total);
                nv2a_d3d_kick();
                /* A failed native view must retry within the same kick. */
                if (cases[item].native) {
                    fail_srv = 1;
                    CHECK(texture_get(&texture) == NULL && fail_srv == 0);
                    CHECK(s_tex_count == count && s_tex_bytes == bytes);
                }
                view = texture_get(&texture);
                CHECK(view);
                CHECK(texture_get(&texture) == view);
                array = bc_array_view(view, &texture_desc);
                CHECK(array);
                ID3D11ShaderResourceView_Release(array);
                ID3D11ShaderResourceView_GetDesc(view, &view_desc);
                CHECK(texture_desc.Format == (cases[item].native ? host_formats[format] : DXGI_FORMAT_B8G8R8A8_UNORM));
                CHECK(view_desc.Format == texture_desc.Format);
                CHECK(view_desc.ViewDimension == (texture.cube ? D3D11_SRV_DIMENSION_TEXTURECUBE : D3D11_SRV_DIMENSION_TEXTURE2D));
                levels = host_levels(&layout, &texture);
                CHECK(texture_desc.MipLevels == levels && texture_desc.ArraySize == layout.faces);
                width = texture.width; height = texture.height;
                for (mip = 0; mip < levels; mip++) {
                    expected_bytes += cases[item].native ? (size_t)((width + 3) / 4) * ((height + 3) / 4) * layout.bpp * layout.faces
                                                       : (size_t)width * height * 4 * layout.faces;
                    if (width > 1) width /= 2;
                    if (height > 1) height /= 2;
                }
                CHECK(s_tex_count == count + 1 && s_tex_bytes == bytes + expected_bytes);
                reference = bc_reference(&texture, &layout);
                CHECK(reference && bc_compare(view, reference, cases[item].native, &texture, &layout, snapshot) == 0);
                if (cases[item].native) {
                    for (i = 0; i < layout.total; i++) ram[texture.addr + i] ^= (uint8_t)(0x5B + i * 17);
                    /* Cached draws in this kick retain the old upload. A new
                     * kick must hash all bytes and refresh this same resource. */
                    CHECK(texture_get(&texture) == view);
                    CHECK(bc_compare(view, reference, 1, &texture, &layout, snapshot) == 0);
                    ID3D11ShaderResourceView_Release(reference);
                    nv2a_d3d_kick();
                    CHECK(texture_get(&texture) == view);
                    CHECK(s_tex_count == count + 1 && s_tex_bytes == bytes + expected_bytes);
                    reference = bc_reference(&texture, &layout);
                    memcpy(snapshot, ram + texture.addr, layout.total);
                    CHECK(reference && bc_compare(view, reference, 1, &texture, &layout, snapshot) == 0);
                }
                ID3D11ShaderResourceView_Release(reference);
                free(snapshot);
            }
    printf("PASS: BC1/2/3 GPU sampling, all mips/faces, filtering, alignment, bytes, fallback and epoch/retry; max RGB %.4f/255, alpha %.4f/255\n",
           bc_max_rgb_error, bc_max_alpha_error);
    return 0;
}

static volatile uint64_t bc_benchmark_hash;

static int bc_benchmark_drain(ID3D11Query *done)
{
    DWORD start = GetTickCount();
    HRESULT hr;
    ID3D11DeviceContext_End(s_ctx, (ID3D11Asynchronous *)done);
    ID3D11DeviceContext_Flush(s_ctx);
    do {
        hr = ID3D11DeviceContext_GetData(s_ctx, (ID3D11Asynchronous *)done, NULL, 0, 0);
        CHECK(hr != S_FALSE || GetTickCount() - start < 5000);
        if (hr == S_FALSE) SwitchToThread();
    } while (hr == S_FALSE);
    CHECK(SUCCEEDED(hr));
    return 0;
}

/* Compare CPU hashing + update calls on already-created resources. The BGRA
 * reference retains the pre-BC decoder/upload loop. GPU completion is drained
 * after each sample, outside its timing, so this does not claim GPU/frame time. */
static double bc_benchmark_upload(const NvD3DTexture *t, const TexLayout *layout,
                                   ID3D11Texture2D *bgra, uint32_t *scratch,
                                   unsigned repetitions, int native)
{
    LARGE_INTEGER frequency;
    int64_t start, end;
    unsigned i, mip;
    QueryPerformanceFrequency(&frequency);
    start = qpc();
    for (i = 0; i < repetitions; i++) {
        ram[t->addr]++;
        if (native) {
            nv2a_d3d_kick();
            if (!texture_get(t)) return -1;
        } else {
            bc_benchmark_hash ^= hash_mem(guest(t->addr), layout->total);
            for (mip = 0; mip < (unsigned)layout->levels; mip++) {
                decode_level(t, layout, guest(t->addr) + layout->level_off[mip], mip, NULL, scratch);
                ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)bgra, mip, NULL,
                                                       scratch, layout->level_w[mip] * 4, 0);
            }
        }
    }
    end = qpc();
    return (end - start) * 1000.0 / frequency.QuadPart / repetitions;
}

static int benchmark_native_bc(void)
{
    const unsigned formats[] = {0x0C,0x0E,0x0F};
    D3D11_QUERY_DESC query_desc = {D3D11_QUERY_EVENT, 0};
    ID3D11Query *done = NULL;
    uint32_t state = 0xABF010CDu, *scratch = malloc(1024 * 1024 * 4);
    unsigned item, sample, i, j;
    CHECK(scratch);
    CHECK(SUCCEEDED(ID3D11Device_CreateQuery(s_dev, &query_desc, &done)));
    for (item = 0; item < 3; item++) {
        NvD3DTexture texture = {0};
        TexLayout layout;
        ID3D11Texture2D *bgra;
        double before[7], selected[7];
        texture.addr = 0x20000 + item * 0x200000;
        texture.color = formats[item];
        texture.width = texture.height = 1024; texture.levels = 11;
        texture.format = (texture.color << 8) | (texture.levels << 16);
        CHECK(tex_layout(&texture, &layout));
        for (i = 0; i < layout.total; i++) ram[texture.addr + i] = (uint8_t)random_word(&state);
        CHECK(texture_get(&texture));
        bgra = make_texture(1024, 1024, 11, 1, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, 0);
        CHECK(bgra);
        CHECK(bc_benchmark_upload(&texture, &layout, bgra, scratch, 1, 0) > 0);
        CHECK(bc_benchmark_drain(done) == 0);
        CHECK(bc_benchmark_upload(&texture, &layout, bgra, scratch, 1, 1) > 0);
        CHECK(bc_benchmark_drain(done) == 0);
        for (sample = 0; sample < 7; sample++) {
            /* Alternate measurement order to reduce warm-up/clock bias. */
            if (sample & 1) {
                selected[sample] = bc_benchmark_upload(&texture, &layout, bgra, scratch, 8, 1);
                CHECK(bc_benchmark_drain(done) == 0);
                before[sample] = bc_benchmark_upload(&texture, &layout, bgra, scratch, 8, 0);
            } else {
                before[sample] = bc_benchmark_upload(&texture, &layout, bgra, scratch, 8, 0);
                CHECK(bc_benchmark_drain(done) == 0);
                selected[sample] = bc_benchmark_upload(&texture, &layout, bgra, scratch, 8, 1);
            }
            CHECK(bc_benchmark_drain(done) == 0);
            CHECK(before[sample] > 0 && selected[sample] > 0);
        }
        for (i = 0; i < 7; i++)
            for (j = i + 1; j < 7; j++) {
                double value;
                if (before[j] < before[i]) { value = before[i]; before[i] = before[j]; before[j] = value; }
                if (selected[j] < selected[i]) { value = selected[i]; selected[i] = selected[j]; selected[j] = value; }
            }
        printf("UPLOAD DXT%u 1024x1024 full11-mip chain: BGRA %.3f ms, native %.3f ms CPU/update (%.2fx); bytes %llu -> %llu\n",
               item == 0 ? 1 : item == 1 ? 3 : 5, before[3], selected[3], before[3] / selected[3],
               (unsigned long long)5592404, (unsigned long long)layout.total);
        ID3D11Texture2D_Release(bgra);
    }
    ID3D11Query_Release(done);
    free(scratch);
    return 0;
}
