/* Programmable vertex shader translation and input-layout caching, using the
 * real d3d8_vsh.c. The layout half runs on D3D11's WARP device; no game data
 * or window is needed.
 *
 *   consts   - a c# above the 192-entry constant file reads as zero (not c[0]),
 *              and c#191 still reads c[191]
 *   wire     - independent wire fields, masks and opcode HLSL compilation
 *   gpu      - paired-unit pre-write operand value through WARP readback
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

/* Independently packed NV2A wire MOV oPos.xyzw, c[index].xyzw. */
static void make_const_mov(DWORD insn[4], uint32_t const_idx)
{
    insn[0] = 0;
    insn[1] = 0x0020001Bu | (const_idx << 13);
    insn[2] = 0x0C000000u;
    insn[3] = 0x0000F801u;
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

static void test_wire(void)
{
    DWORD wire[] = {0,0x0037E01Bu,0x0C000000u,0x0000F801u};
    NV2AVshProgram program;
    char hlsl[16384];
    ID3DBlob *code=NULL,*errors=NULL;
    d3d8_vsh_parse(wire,1,&program);
    CHECK(!program.unsupported && program.length==1);
    CHECK(program.insns[0].mac_op==NV2A_VSH_MAC_MOV);
    CHECK(program.insns[0].const_index==191);
    CHECK(program.insns[0].mac_src[0].reg_type==NV2A_VSH_REG_CONST);
    CHECK(program.insns[0].mac_src[0].swizzle.w==3);
    CHECK(program.insns[0].mac_dst.output_reg==NV2A_VSH_OUT_POS);
    CHECK(program.insns[0].mac_dst.output_mask==15 && program.insns[0].mac_dst.write_mask==0);
    CHECK(program.inputs_read==0);
    wire[0]=0xFFFFFFFFu;
    d3d8_vsh_parse(wire,1,&program);
    CHECK(!program.unsupported && program.insns[0].const_index==191);
    int n=d3d8_vsh_generate_hlsl(&program,hlsl,sizeof hlsl);
    CHECK(n>0);
    HRESULT hr=D3DCompile(hlsl,n,"wire",NULL,NULL,"main","vs_5_0",0,0,&code,&errors);
    if (FAILED(hr)) fprintf(stderr,"wire compile: %s\n",errors?(char *)ID3D10Blob_GetBufferPointer(errors):"unknown");
    CHECK(SUCCEEDED(hr));
    if(code)ID3D10Blob_Release(code);
    if(errors)ID3D10Blob_Release(errors);
    /* Compile every opcode from independently packed source/destination fields.
     * This checks emitted syntax, not numerical conformance of every opcode. */
    for (unsigned op=1;op<=20;op++) {
        unsigned mac=op<=13?op:0, ilu=op>13?op-13:0;
        wire[0]=0;wire[1]=(mac<<21)|(ilu<<25)|0x201Bu;
        wire[2]=0x0C000000u|(0x1Bu<<17)|(3u<<11)|(0x1Bu<<2);
        wire[3]=0x3000F819u|(ilu?4u:0u);
        if(mac==13)wire[3]&=~0xF000u;
        d3d8_vsh_parse(wire,1,&program);
        CHECK(!program.unsupported);
        CHECK((unsigned)program.insns[0].mac_op==mac && (unsigned)program.insns[0].ilu_op==ilu);
        n=d3d8_vsh_generate_hlsl(&program,hlsl,sizeof hlsl);CHECK(n>0);
        if(n<=0)continue;
        code=errors=NULL;
        hr=D3DCompile(hlsl,n,"opcode-wire",NULL,NULL,"main","vs_5_0",0,0,&code,&errors);
        if(FAILED(hr))fprintf(stderr,"opcode %u: %s\n",op,errors?(char *)ID3D10Blob_GetBufferPointer(errors):"unknown");
        CHECK(SUCCEEDED(hr));
        if(code)ID3D10Blob_Release(code);
        if(errors)ID3D10Blob_Release(errors);
    }
    /* Paired MOV: MAC R2.x, ILU R1.y and oD0.z, with C=R2. */
    wire[1]=0x0237E01Bu;wire[2]=0x0C00006Cu;wire[3]=0x9824281Du;
    d3d8_vsh_parse(wire,1,&program);
    CHECK(!program.unsupported);
    CHECK(program.insns[0].mac_dst.temp_reg==2 && program.insns[0].mac_dst.write_mask==8);
    CHECK(program.insns[0].ilu_dst.temp_reg==1 && program.insns[0].ilu_dst.write_mask==4);
    CHECK(program.insns[0].ilu_src.reg_type==NV2A_VSH_REG_TEMP && program.insns[0].ilu_src.reg_index==2);
    CHECK(program.insns[0].ilu_dst.output_reg==NV2A_VSH_OUT_D0 && program.insns[0].ilu_dst.output_mask==2);
    CHECK(program.insns[0].mac_dst.output_mask==0);
    wire[3]=(wire[3]&~0x00F00000u)|0x00100000u;
    d3d8_vsh_parse(wire,1,&program);
    CHECK(!program.unsupported && program.insns[0].mac_dst.write_mask==0);
    /* Relative constants, an actual input and cross-word C temporary index. */
    wire[1]=0x00600C1Bu;wire[2]=0x0800006Eu;wire[3]=0xD000F803u;
    d3d8_vsh_parse(wire,1,&program);
    CHECK(!program.unsupported && program.inputs_read==(1u<<6));
    CHECK(program.insns[0].mac_src[0].reg_type==NV2A_VSH_REG_INPUT);
    CHECK(program.insns[0].mac_src[2].reg_index==11);
    make_const_mov(wire,191);wire[3]|=2;
    d3d8_vsh_parse(wire,1,&program);
    CHECK(!program.unsupported && program.insns[0].mac_src[0].rel_addr);
    /* Preserve original API output on unsupported constant-file writes. */
    wire[3]=0x0000F001u;
    d3d8_vsh_parse(wire,1,&program);
    CHECK(program.unsupported && program.insns[0].mac_dst.constant_reg==0);
    CHECK(d3d8_vsh_generate_hlsl(&program,hlsl,sizeof hlsl)==-1);
    DWORD handle=0xBAADu;
    CHECK(d3d8_vsh_create_shader(wire,1,&handle)==E_NOTIMPL && handle==0xBAADu);
    make_const_mov(wire,191);
    CHECK(d3d8_vsh_create_shader(wire,1,&handle)==S_OK && handle>=0x10000u);
    CHECK(d3d8_vsh_delete_shader(handle)==S_OK);
}

#define REQUIRE_GPU(call) do { HRESULT hr_=(call); if(FAILED(hr_)) { \
    fprintf(stderr,"GPU HRESULT %08lx: %s\n",(unsigned long)hr_,#call);exit(1); } } while(0)

static void test_program_gpu(void)
{
    /* Position=c0; R2=c1; paired MAC R2.x=c191 / ILU oD0.x=old R2.x. */
    const DWORD wire[] = {
        0,0x0020001Bu,0x0C000000u,0x0000F800u,
        0,0x0020201Bu,0x0C000000u,0x0F200000u,
        0,0x0237E01Bu,0x0C00006Cu,0x9824881Du
    };
    const char ps_text[]="float4 main(float4 position:SV_Position,float4 color:COLOR0):SV_Target{return color;}";
    NV2AVshProgram program;
    NV2AVSConstants constants;
    char hlsl[16384];
    ID3DBlob *vs_code=NULL,*ps_code=NULL,*errors=NULL;
    ID3D11VertexShader *shader=NULL;
    ID3D11PixelShader *pixel=NULL;
    ID3D11Texture2D *target=NULL,*readback=NULL;
    ID3D11RenderTargetView *rtv=NULL;
    ID3D11Buffer *cb=NULL;
    D3D11_TEXTURE2D_DESC td={0};
    D3D11_BUFFER_DESC bd={0};
    D3D11_SUBRESOURCE_DATA data={0};
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_VIEWPORT vp={0,0,1,1,0,1};
    const float clear[]={1,0,1,1};
    const BYTE expected[]={64,0,0,255};
    d3d8_vsh_parse(wire,3,&program);
    CHECK(!program.unsupported && program.length==3);
    int n=d3d8_vsh_generate_hlsl(&program,hlsl,sizeof hlsl);
    CHECK(n>0);
    HRESULT hr=D3DCompile(hlsl,n,"paired-wire",NULL,NULL,"main","vs_5_0",0,0,&vs_code,&errors);
    if(FAILED(hr)) fprintf(stderr,"paired compile: %s\n",errors?(char *)ID3D10Blob_GetBufferPointer(errors):"unknown");
    REQUIRE_GPU(hr);
    if(errors)ID3D10Blob_Release(errors);
    REQUIRE_GPU(D3DCompile(ps_text,sizeof(ps_text)-1,"pixel",NULL,NULL,"main","ps_5_0",0,0,&ps_code,NULL));
    REQUIRE_GPU(ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(vs_code),ID3D10Blob_GetBufferSize(vs_code),NULL,&shader));
    REQUIRE_GPU(ID3D11Device_CreatePixelShader(device,ID3D10Blob_GetBufferPointer(ps_code),ID3D10Blob_GetBufferSize(ps_code),NULL,&pixel));
    memset(&constants,0,sizeof constants);
    constants.c[0][3]=1; constants.c[1][0]=0.25f; constants.c[191][0]=0.75f;
    bd.ByteWidth=sizeof constants;bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER;data.pSysMem=&constants;
    REQUIRE_GPU(ID3D11Device_CreateBuffer(device,&bd,&data,&cb));
    td.Width=td.Height=td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
    REQUIRE_GPU(ID3D11Device_CreateTexture2D(device,&td,NULL,&target));
    REQUIRE_GPU(ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource *)target,NULL,&rtv));
    td.BindFlags=0;td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    REQUIRE_GPU(ID3D11Device_CreateTexture2D(device,&td,NULL,&readback));
    ID3D11DeviceContext_ClearState(context);
    ID3D11DeviceContext_ClearRenderTargetView(context,rtv,clear);
    ID3D11DeviceContext_OMSetRenderTargets(context,1,&rtv,NULL);
    ID3D11DeviceContext_RSSetViewports(context,1,&vp);
    ID3D11DeviceContext_VSSetShader(context,shader,NULL,0);
    ID3D11DeviceContext_VSSetConstantBuffers(context,1,1,&cb);
    ID3D11DeviceContext_PSSetShader(context,pixel,NULL,0);
    ID3D11DeviceContext_IASetPrimitiveTopology(context,D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_Draw(context,1,0);
    ID3D11DeviceContext_CopyResource(context,(ID3D11Resource *)readback,(ID3D11Resource *)target);
    REQUIRE_GPU(ID3D11DeviceContext_Map(context,(ID3D11Resource *)readback,0,D3D11_MAP_READ,0,&mapped));
    if(memcmp(mapped.pData,expected,4)) {
        const BYTE *v=mapped.pData;
        fprintf(stderr,"paired GPU: %u,%u,%u,%u expected 64,0,0,255\n",v[0],v[1],v[2],v[3]);failures++;
        fprintf(stderr,"%s\n",hlsl);

    }
    ID3D11DeviceContext_Unmap(context,(ID3D11Resource *)readback,0);
    ID3D11DeviceContext_ClearState(context);
    ID3D11Buffer_Release(cb);ID3D11Texture2D_Release(readback);ID3D11RenderTargetView_Release(rtv);
    ID3D11Texture2D_Release(target);ID3D11PixelShader_Release(pixel);ID3D11VertexShader_Release(shader);
    ID3D10Blob_Release(vs_code);ID3D10Blob_Release(ps_code);
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
    if (strcmp(which,"wire")==0 || strcmp(which,"all")==0) test_wire();

    if (strcmp(which, "layouts") == 0 || strcmp(which,"gpu")==0 || strcmp(which, "all") == 0) {
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0,
                               D3D11_SDK_VERSION, &device, NULL, &context);
        if (FAILED(hr)) {
            fprintf(stderr, "no WARP device: 0x%08lX\n", hr);
            return 1;
        }
        if (strcmp(which,"layouts")==0 || strcmp(which,"all")==0) test_layouts();
        if (strcmp(which,"gpu")==0 || strcmp(which,"all")==0) test_program_gpu();
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
