/* Exercise the real sampler table/fast requests, including native GPU taps. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned create_calls;
static int fail_sampler, fail_srv, fail_alloc;
static size_t fail_alloc_size;
static HRESULT create_sampler(ID3D11Device *dev, const D3D11_SAMPLER_DESC *desc,
                              ID3D11SamplerState **out)
{
    create_calls++;
    if (fail_sampler) { fail_sampler--; *out = NULL; return E_OUTOFMEMORY; }
    return dev->lpVtbl->CreateSamplerState(dev, desc, out);
}
static HRESULT create_srv(ID3D11Device *dev, ID3D11Resource *res,
                          const D3D11_SHADER_RESOURCE_VIEW_DESC *desc,
                          ID3D11ShaderResourceView **out)
{
    if (fail_srv) { fail_srv--; *out = NULL; return E_OUTOFMEMORY; }
    return dev->lpVtbl->CreateShaderResourceView(dev, res, desc, out);
}
static void *allocate(size_t bytes)
{
    if (fail_alloc && bytes == fail_alloc_size) { fail_alloc--; return NULL; }
    return malloc(bytes);
}
#undef ID3D11Device_CreateSamplerState
#undef ID3D11Device_CreateShaderResourceView
#define ID3D11Device_CreateSamplerState create_sampler
#define ID3D11Device_CreateShaderResourceView create_srv
#define malloc allocate
#include "../../src/video/nv2a_d3d11.c"
#include "../nv2a_test_host.h"
#undef malloc

static uint8_t ram[0x100000];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
HWND xbox_FramebufferWindowHandle(void) { return NULL; }
int xbox_FramebufferScanSource(uint32_t *va, uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t draws) {}
const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4] { return NULL; }
const float (*nv2a_vsh_constants(uint32_t *version))[4] { return NULL; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)

#define TAPS 128
static ID3D11ComputeShader *compute;
static ID3D11Buffer *output, *staging;
static ID3D11UnorderedAccessView *uav;
static ID3D11ShaderResourceView *sample_view;

static void make_sampling_fixture(void)
{
    static const char shader[] =
        "Texture2D<float4> source:register(t0); SamplerState state:register(s0);"
        "RWBuffer<uint> output:register(u0);"
        "[numthreads(128,1,1)] void main(uint3 id:SV_DispatchThreadID) {"
        " uint i=id.x; float2 uv=i==0?float2(.0625,.0625):"
        " float2(-.375+(i%8)*.25,-.25+((i/8)%4)*.5);"
        " float lod=i==0?0:(i/32)*1.25;"
        " uint4 c=(uint4)round(saturate(source.SampleLevel(state,uv,lod))*255);"
        " output[i]=c.b|(c.g<<8)|(c.r<<16)|(c.a<<24); }";
    ID3DBlob *code=NULL, *errors=NULL;
    ID3D11Texture2D *texture;
    D3D11_SUBRESOURCE_DATA data[4]={0};
    D3D11_BUFFER_DESC bd={0};
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud={0};
    uint32_t pixels[4][64];
    unsigned lv,x,y;
    CHECK(SUCCEEDED(D3DCompile(shader,sizeof shader-1,"sampler_taps",NULL,NULL,"main","cs_5_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors)));
    CHECK(SUCCEEDED(ID3D11Device_CreateComputeShader(s_dev,ID3D10Blob_GetBufferPointer(code),
                    ID3D10Blob_GetBufferSize(code),NULL,&compute)));
    ID3D10Blob_Release(code); if(errors) ID3D10Blob_Release(errors);
    for(lv=0;lv<4;lv++) {
        unsigned w=8>>lv;
        for(y=0;y<w;y++) for(x=0;x<w;x++)
            pixels[lv][y*w+x]=0xFF000000u | (((x+y+lv)&1)?0xFF0000u:0x00FF00u) |
                                   ((lv*47+x*11+y*3)&255);
        data[lv].pSysMem=pixels[lv]; data[lv].SysMemPitch=w*4;
    }
    {
        D3D11_TEXTURE2D_DESC td={0};
        td.Width=td.Height=8;td.MipLevels=4;td.ArraySize=1;td.SampleDesc.Count=1;
        td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.Usage=D3D11_USAGE_IMMUTABLE;
        td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev,&td,data,&texture)));
        CHECK(SUCCEEDED(ID3D11Device_CreateShaderResourceView(s_dev,(ID3D11Resource *)texture,NULL,&sample_view)));
        ID3D11Texture2D_Release(texture);
    }
    bd.ByteWidth=TAPS*4;bd.Usage=D3D11_USAGE_DEFAULT;bd.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(s_dev,&bd,NULL,&output)));
    ud.Format=DXGI_FORMAT_R32_UINT;ud.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;ud.Buffer.NumElements=TAPS;
    CHECK(SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(s_dev,(ID3D11Resource *)output,&ud,&uav)));
    bd.Usage=D3D11_USAGE_STAGING;bd.BindFlags=0;bd.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(ID3D11Device_CreateBuffer(s_dev,&bd,NULL,&staging)));
}

static void sample(ID3D11SamplerState *state, uint32_t values[TAPS])
{
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11UnorderedAccessView *none_uav=NULL;
    ID3D11ShaderResourceView *none_srv=NULL;
    ID3D11SamplerState *none_sampler=NULL;
    CHECK(state);
    ID3D11DeviceContext_CSSetShader(s_ctx,compute,NULL,0);
    ID3D11DeviceContext_CSSetShaderResources(s_ctx,0,1,&sample_view);
    ID3D11DeviceContext_CSSetSamplers(s_ctx,0,1,&state);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(s_ctx,0,1,&uav,NULL);
    ID3D11DeviceContext_Dispatch(s_ctx,1,1,1);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(s_ctx,0,1,&none_uav,NULL);
    ID3D11DeviceContext_CSSetShaderResources(s_ctx,0,1,&none_srv);
    ID3D11DeviceContext_CSSetSamplers(s_ctx,0,1,&none_sampler);
    ID3D11DeviceContext_CSSetShader(s_ctx,NULL,NULL,0);
    ID3D11DeviceContext_CopyResource(s_ctx,(ID3D11Resource *)staging,(ID3D11Resource *)output);
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(s_ctx,(ID3D11Resource *)staging,0,D3D11_MAP_READ,0,&m)));
    memcpy(values,m.pData,TAPS*4);
    ID3D11DeviceContext_Unmap(s_ctx,(ID3D11Resource *)staging,0);
}

static void compare(ID3D11SamplerState *expected, ID3D11SamplerState *actual)
{
    D3D11_SAMPLER_DESC a={0},b={0};uint32_t old[TAPS],now[TAPS];
    CHECK(expected && actual);
    ID3D11SamplerState_GetDesc(expected,&a);ID3D11SamplerState_GetDesc(actual,&b);
    CHECK(!memcmp(&a,&b,sizeof a));sample(expected,old);sample(actual,now);
    CHECK(!memcmp(old,now,sizeof old));
}

static NvD3DTexture request(unsigned i)
{
    NvD3DTexture t={0};
    static const unsigned modes[]={1,2,3,4};
    t.filter=((1+i%7)<<16)|((1+(i/7)%2)<<24)|((i&1)?0x1F80:0x80);
    t.address=modes[i%4]|(modes[(i/4)%4]<<8)|(modes[(i/16)%4]<<16);
    t.control0=(((i%4)*256)<<18)|((((i/4)%4)*256)<<6);
    t.border=0x80336699u+i*0x01010203u;
    return t;
}

static void test_pixels(void)
{
    unsigned i,repeat,calls;
    for(i=0;i<112;i++) {
        NvD3DTexture t=request(i);int levels=(i%5)+1,reduce=(i/7)%2;
        ID3D11SamplerState *old=sampler(&t,levels,reduce);
        ID3D11SamplerState *now=sampler_stage(i%4,&t,levels,reduce);
        compare(old,now);calls=create_calls;
        for(repeat=0;repeat<8;repeat++) CHECK(sampler_stage(i%4,&t,levels,reduce)==now);
        CHECK(create_calls==calls);
        /* Changes to ignored raw bits must conservatively refresh the request,
         * while retaining the existing key/descriptor semantics. */
        t.filter^=0x8000;now=sampler_stage(i%4,&t,levels,reduce);
        CHECK(s_sampler_request[i%4].filter==t.filter);compare(sampler(&t,levels,reduce),now);
    }
    {
        NvD3DTexture t={0};uint32_t values[TAPS];
        t.filter=(1u<<16)|(1u<<24);t.address=0x030303;
        sample(sampler_stage(0,&t,1,0),values);CHECK(values[0]==0xFF00FF00u);
    }
    puts("PASS: 224 exact sampler descriptors and 28672 paired native GPU taps; four stages, addressing/filter/bias/LOD/border/reduction, conservative raw-input changes");
}

static void test_eviction(void)
{
    NvD3DTexture t=request(2),other=request(4);
    ID3D11SamplerState *old=sampler_stage(0,&t,4,0),*now;
    StateSlot *slot=s_sampler_request[0].slot;
    uint64_t key=s_sampler_request[0].key;unsigned i;
    CHECK(old);ID3D11SamplerState_AddRef(old);
    for(i=0;i<32768 && slot->key==key;i++) {
        other.border=0xFF000000u+i;CHECK(sampler(&other,4,0));
    }
    CHECK(i>STATE_SLOTS && slot->key!=key);
    now=sampler_stage(0,&t,4,0);CHECK(now);compare(old,now);
    CHECK(s_sampler_request[0].slot->key==key);
    ID3D11SamplerState_Release(old);
    printf("PASS: actual full-table eviction after %u requests, slot validation/recovery and exact GPU pixels\n",i);
}

static void test_failure(void)
{
    NvD3DTexture t=request(81);ID3D11SamplerState *now;unsigned calls=create_calls;
    fail_sampler=1;CHECK(!sampler_stage(3,&t,4,1));CHECK(create_calls==calls+1);
    CHECK(s_sampler_request[3].slot && !s_sampler_request[3].slot->obj);
    now=sampler_stage(3,&t,4,1);CHECK(now && create_calls==calls+2);
    compare(sampler(&t,4,1),now);CHECK(sampler_stage(3,&t,4,1)==now);
    puts("PASS: failed creation publishes no fast hit; same-request retry and native GPU pixels match");
}

static int old_levels(const NvD3DTexture *t)
{
    TexLayout l;return tex_layout(t,&l)?l.levels:1;
}
static ID3D11ShaderResourceView *check_levels(const NvD3DTexture *t)
{
    int levels=-7;ID3D11ShaderResourceView *v=texture_get_with_levels(t,&levels);
    CHECK(levels==old_levels(t));return v;
}
static NvD3DState pipeline_state(const NvD3DTexture *t)
{
    NvD3DState st={0};st.color_addr=0x10000;st.color_pitch=64*4;
    st.surface_format=0x28;st.clip_w=st.clip_h=64;st.color_mask=0x01010101;
    st.tex_used=1;st.tex[0]=*t;return st;
}
static void check_pipeline(NvD3DState *st, int levels, int reduce, int has_view)
{
    Surface *target;uint32_t w,h,z;ID3D11SamplerState *bound=NULL;
    ID3D11ShaderResourceView *view=NULL;
    D3D11_SAMPLER_DESC actual={0},expected={0};
    CHECK(setup_pipeline(st,NV_D3D_TRIANGLES,&target,&w,&h,&z));
    ID3D11DeviceContext_PSGetSamplers(s_ctx,0,1,&bound);
    ID3D11DeviceContext_PSGetShaderResources(s_ctx,0,1,&view);
    CHECK((view!=NULL)==has_view);
    ID3D11SamplerState_GetDesc(bound,&actual);
    ID3D11SamplerState_GetDesc(has_view?sampler(&st->tex[0],levels,reduce):s_linear_clamp,&expected);
    CHECK(!memcmp(&actual,&expected,sizeof actual));
    if(view) ID3D11ShaderResourceView_Release(view);ID3D11SamplerState_Release(bound);
}
static void test_levels(void)
{
    NvD3DTexture t={0};NvD3DState st;ID3D11ShaderResourceView *v;
    unsigned i;Surface *rt;
    for(i=0;i<0x70000/4;i++) ((uint32_t *)(ram+0x20000))[i]=0xFF00FF00u;
    t.addr=0x20000;t.color=0x06;t.width=t.height=8;t.levels=4;t.format=0x00040600;
    t.filter=(6u<<16)|(1u<<24);t.control0=3*256<<6;t.address=0x030303;
    CHECK(check_levels(&t));CHECK(check_levels(&t)); /* same-kick cached view */
    st=pipeline_state(&t);check_pipeline(&st,old_levels(&t),0,1);
    t.levels=0;t.format++;CHECK(check_levels(&t));
    t.levels=15;t.format++;CHECK(check_levels(&t));
    t.addr=0x24000;t.cube=1;t.format++;CHECK(check_levels(&t));t.cube=0;t.format++;
    t.width=0;CHECK(!check_levels(&t));t.width=8;
    t.color=0xFF;CHECK(!check_levels(&t));t.color=0x06;
    t.addr=0x07FFFFF0;CHECK(!check_levels(&t));t.addr=0x28000;
    t.levels=2;t.format++;fail_srv=1;CHECK(!check_levels(&t));CHECK(!fail_srv);
    CHECK(check_levels(&t));
    /* Existing upload retained on a failed scratch allocation. Generated
     * mips reuse this scratch, so release it to exercise the retry path. */
    t.addr=0x30000;t.format++;v=check_levels(&t);CHECK(v);
    free(s_up_pixels);s_up_pixels=NULL;s_up_cap=0;
    ram[t.addr]^=0x10;s_epoch++;fail_alloc_size=8*8*4;fail_alloc=1;
    CHECK(check_levels(&t)==v);CHECK(!fail_alloc);CHECK(check_levels(&t)==v);
    free(s_up_pixels);s_up_pixels=NULL;s_up_cap=0;
    t.addr=0x38000;t.format++;fail_alloc=1;CHECK(!check_levels(&t));CHECK(!fail_alloc);
    CHECK(check_levels(&t));
    t.color=0x12;t.pitch=8*4;t.levels=15;t.addr=0x40000;t.format++;
    CHECK(check_levels(&t));CHECK(old_levels(&t)==1);
    st=pipeline_state(&t);check_pipeline(&st,1,0,1);
    st.tex_used=0;check_pipeline(&st,1,0,0);
    st.tex_used=1;st.tex[0].control0=0;check_pipeline(&st,1,0,1);
    st.tex[0].addr=0;check_pipeline(&st,1,0,0);
    st.tex[0].addr=t.addr;st.tex[0].width=0;check_pipeline(&st,1,0,0);
    st.tex[0].width=8;st.tex[0].color=0xFF;check_pipeline(&st,1,0,0);
    /* Render-target aliases retain their separate, existing level policy. */
    t.addr=0x50000;t.color=0x06;t.pitch=32;t.width=t.height=8;t.levels=4;t.format++;
    rt=surface_get(t.addr,32,8,8,8,1);CHECK(rt);
    CHECK(surface_get(t.addr+8*8*4,16,8,4,4,1));
    CHECK(surface_get(t.addr+(8*8+4*4)*4,8,8,2,2,1));
    CHECK(surface_get(t.addr+(8*8+4*4+2*2)*4,4,8,1,1,1));
    st=pipeline_state(&t);check_pipeline(&st,4,0,1);CHECK(rt->chain_levels==4);
    t.addr=0x60000;t.color=0x12;t.pitch=128;t.width=t.height=32;t.levels=4;
    rt=surface_get(t.addr,128,8,32,32,0);CHECK(rt);
    st=pipeline_state(&t);check_pipeline(&st,1,0,1);
    st.clip_w=st.clip_h=16;st.color_pitch=64;st.color_addr=0x18000;
    check_pipeline(&st,1,1,1);CHECK(rt->mip_srv);
    puts("PASS: supplied levels match old layout on defaults/clamps/cache hits/bounds/SRV/mip failures, disabled stages/control0 and actual RT/chain/reduction aliases");
}

static volatile uintptr_t sink;
static __declspec(noinline) ID3D11SamplerState *old_request(unsigned stage,const NvD3DTexture *t,int levels,int reduce)
{ return sampler(t,levels,reduce); }
static __declspec(noinline) ID3D11SamplerState *new_request(unsigned stage,const NvD3DTexture *t,int levels,int reduce)
{ return sampler_stage(stage,t,levels,reduce); }
typedef ID3D11SamplerState *(*Lookup)(unsigned,const NvD3DTexture *,int,int);
static __declspec(noinline) double time_lookup(Lookup lookup,NvD3DTexture *t,unsigned stages,unsigned iterations)
{
    LARGE_INTEGER f;int64_t begin;unsigned i;QueryPerformanceFrequency(&f);begin=qpc();
    for(i=0;i<iterations;i++) { unsigned stage=i%stages;sink^=(uintptr_t)lookup(stage,&t[stage],4,0); }
    return (qpc()-begin)*1e9/f.QuadPart/iterations;
}
static void benchmark(void)
{
    NvD3DTexture t[4];double old[9],now[9];unsigned stages,sample,i,j;
    for(i=0;i<4;i++) {t[i]=request(i*19);CHECK(sampler_stage(i,&t[i],4,0));}
    for(stages=1;stages<=4;stages++) {
        for(sample=0;sample<9;sample++) {
            if(sample&1) {now[sample]=time_lookup(new_request,t,stages,2000000);old[sample]=time_lookup(old_request,t,stages,2000000);}
            else {old[sample]=time_lookup(old_request,t,stages,2000000);now[sample]=time_lookup(new_request,t,stages,2000000);}
        }
        for(i=0;i<9;i++) for(j=i+1;j<9;j++) {double v;
            if(old[j]<old[i]){v=old[i];old[i]=old[j];old[j]=v;}
            if(now[j]<now[i]){v=now[i];now[i]=now[j];now[j]=v;}}
        printf("SAMPLER %u alternating stages, median of 9 x 2000000 calls: uncached request %.2f ns (%.3f ms total), retained request %.2f ns (%.3f ms total); %.3f ms saved/3200 calls\n",
               stages,old[4],old[4]*2,now[4],now[4]*2,(old[4]-now[4])*3200/1e6);
    }
}
int main(int argc,char **argv)
{
    CHECK(argc==2);_putenv_s("RECOMP_MSAA","1");_putenv_s("RECOMP_RENDER_SCALE","1");
    _putenv_s("RECOMP_PS_SPEC","0");_putenv_s("RECOMP_FPS_LIMIT","0");
    CHECK(nv2a_d3d_init());
    if(!strcmp(argv[1],"benchmark")) {benchmark();return 0;}
    make_sampling_fixture();
    if(!strcmp(argv[1],"pixels")) test_pixels();
    else if(!strcmp(argv[1],"eviction")) test_eviction();
    else if(!strcmp(argv[1],"failure")) test_failure();
    else if(!strcmp(argv[1],"levels")) test_levels();
    else CHECK(0);
    return 0;
}
