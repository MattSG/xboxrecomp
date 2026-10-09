/* Exercise real draw setup when a sampled surface has new CPU pixels. */
#include "../../src/video/nv2a_d3d11.c"

static uint8_t *ram;
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
HWND xbox_FramebufferWindowHandle(void) { return NULL; }
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t draws) {}
const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4] { return NULL; }
const float (*nv2a_vsh_constants(uint32_t *version))[4] { return NULL; }

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(int argc, char **argv)
{
    NvD3DState st = {0};
    Surface *sample, *target;
    ID3D11RenderTargetView *bound = NULL;
    ID3D11DepthStencilView *depth = NULL;
    D3D11_VIEWPORT vp;
    UINT nvp = 1;
    uint32_t gw, gh, zmax;
    ram = calloc(1, 0x100000);
    CHECK(ram);
    _putenv_s("RECOMP_MSAA", argc > 1 ? argv[1] : "1");
    _putenv_s("RECOMP_RENDER_SCALE", "1");
    _putenv_s("RECOMP_PS_SPEC", "0");
    CHECK(nv2a_d3d_init());
    sample = surface_get(0x20000, 32 * 4, 8, 32, 32, 0);
    CHECK(sample);
    st.color_addr = 0x10000; st.color_pitch = 64 * 4;
    st.surface_format = 0x28; st.clip_w = st.clip_h = 64;
    st.zeta_addr = 0x30000; st.depth_test = st.depth_mask = 1; st.depth_func = 7;
    st.color_mask = 0x01010101;
    st.tex_used = 1;
    st.tex[0].addr = sample->va;
    st.tex[0].color = 0x12; st.tex[0].width = st.tex[0].height = 32;
    st.tex[0].pitch = 32 * 4; st.tex[0].levels = 1;
    CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &target, &gw, &gh, &zmax));
    ID3D11DeviceContext_OMGetRenderTargets(s_ctx, 1, &bound, &depth);
    CHECK(bound == target->rtv);
    CHECK(depth);
    ID3D11DepthStencilView_Release(depth);
    ID3D11RenderTargetView_Release(bound);
    /* CPU change forces surface_sync to draw an upload during texture setup. */
    ram[sample->va] ^= 0xFF;
    s_epoch++;
    CHECK(setup_pipeline(&st, NV_D3D_TRIANGLES, &target, &gw, &gh, &zmax));
    bound = NULL;
    ID3D11DeviceContext_OMGetRenderTargets(s_ctx, 1, &bound, &depth);
    CHECK(bound == target->rtv);
    CHECK(depth);
    ID3D11DepthStencilView_Release(depth);
    ID3D11RenderTargetView_Release(bound);
    ID3D11DeviceContext_RSGetViewports(s_ctx, &nvp, &vp);
    CHECK(nvp == 1 && vp.Width == 64 && vp.Height == 64);
    puts("PASS: sampled-surface upload preserves draw targets and viewport");
    return 0;
}
