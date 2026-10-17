/* Programmable vertex shader translation and input-layout caching, using the
 * real d3d8_vsh.c. The layout half runs on D3D11's WARP device; no game data
 * or window is needed.
 *
 *   consts   - a c# above the 192-entry constant file reads as zero (not c[0]),
 *              and c#191 still reads c[191]
 *   layouts  - 17 distinct layout keys on one shader: the 17th gets its own
 *              layout instead of another key's
 */
#define COBJMACROS
#include "../../src/d3d/d3d8_vsh.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Accessors d3d8_vsh.c calls; the rest of the device layer is not used. ---- */
static ID3D11Device *device;
static ID3D11DeviceContext *context;

ID3D11Device *d3d8_GetD3D11Device(void) { return device; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void) { return context; }
DWORD d3d8_GetCurrentFVF(void) { return 0; }

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

/* Microcode: one MOV o[POS] = c#[const_idx] (src A is a constant, no relative
 * addressing), final instruction. Field positions are the ones d3d8_vsh.c
 * extracts with. */
static void make_const_mov(DWORD insn[4], uint32_t const_idx)
{
    memset(insn, 0, 4 * sizeof(DWORD));
    insn[0] |= (DWORD)NV2A_VSH_MAC_MOV << VSH_FIELD_MAC_OP_START;
    insn[0] |= const_idx << VSH_FIELD_CONST_IDX_START;
    insn[0] |= 2u << VSH_FIELD_SRC_A_TYPE_START;     /* constant register */
    insn[2] |= 0xFu << (VSH_FIELD_MAC_DST_MASK_START - 64);
    insn[3] |= 1u;                                    /* final instruction */
}

static int hlsl_for_const(uint32_t const_idx, char *hlsl, int size)
{
    DWORD insn[4];
    NV2AVshProgram *program = calloc(1, sizeof *program);
    int len;

    make_const_mov(insn, const_idx);
    d3d8_vsh_parse(insn, 1, program);
    len = d3d8_vsh_generate_hlsl(program, hlsl, size);
    free(program);
    return len;
}

static void test_consts(void)
{
    char hlsl[8192];

    CHECK(hlsl_for_const(191, hlsl, sizeof hlsl) > 0);
    CHECK(strstr(hlsl, "c[191]") != NULL);

    CHECK(hlsl_for_const(200, hlsl, sizeof hlsl) > 0);
    CHECK(strstr(hlsl, "c[200]") == NULL);
    CHECK(strstr(hlsl, "c[0]") == NULL);       /* the old clamp aliased c[0] */
    CHECK(strstr(hlsl, "float4(0,0,0,0)") != NULL);
}

static void test_layouts(void)
{
    static const char src[] =
        "float4 main(float3 p : ATTR0) : SV_Position { return float4(p, 1.0f); }";
    ID3DBlob *blob = NULL, *errors = NULL;
    VshCacheEntry entry;
    ID3D11InputLayout *l16;
    uint32_t key16 = 1u | (16u << 16);
    int i;

    HRESULT hr = D3DCompile(src, sizeof src - 1, "layout_test", NULL, NULL, "main",
                            "vs_5_0", 0, 0, &blob, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "shader compile failed: %s\n",
                errors ? (char *)ID3D10Blob_GetBufferPointer(errors) : "?");
        failures++;
        return;
    }

    memset(&entry, 0, sizeof entry);
    entry.inputs_read = 1u;                  /* ATTR0 only */
    entry.vs_blob = blob;

    /* Sixteen distinct keys fill the cache, each with its own layout. */
    for (i = 0; i < 16; i++) {
        ID3D11InputLayout *l = get_cached_layout(&entry, (DWORD)(i << 16));
        CHECK(l != NULL);
        CHECK(entry.layout_masks[i] == (1u | ((uint32_t)i << 16)));
    }
    CHECK(entry.layout_count == 16);

    /* The 17th key must get a layout for its own key, not layouts[0]. */
    l16 = get_cached_layout(&entry, (DWORD)(16 << 16));
    CHECK(l16 != NULL);
    CHECK(entry.layout_count >= 1);
    CHECK(entry.layout_masks[entry.layout_count - 1] == key16);
    CHECK(entry.layouts[entry.layout_count - 1] == l16);

    /* And a repeat of key 16 is served from the cache, not re-created. */
    CHECK(get_cached_layout(&entry, (DWORD)(16 << 16)) == l16);

    for (i = 0; i < entry.layout_count; i++)
        ID3D11InputLayout_Release(entry.layouts[i]);
    ID3D10Blob_Release(blob);
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "all";
    HRESULT hr;

    if (strcmp(which, "consts") == 0 || strcmp(which, "all") == 0)
        test_consts();

    if (strcmp(which, "layouts") == 0 || strcmp(which, "all") == 0) {
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                               D3D11_SDK_VERSION, &device, NULL, &context);
        if (FAILED(hr)) {
            fprintf(stderr, "no WARP device: 0x%08lX\n", hr);
            return 1;
        }
        test_layouts();
        ID3D11DeviceContext_Release(context);
        ID3D11Device_Release(device);
    }

    if (failures) {
        fprintf(stderr, "d3d8_vsh: %d failure(s)\n", failures);
        return 1;
    }
    printf("d3d8_vsh: %s passed\n", which);
    return 0;
}
