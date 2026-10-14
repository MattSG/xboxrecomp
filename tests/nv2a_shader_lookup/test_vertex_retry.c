/* Failed dynamic uploads must never draw stale vertex programs/constants. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_1.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static ID3D11Resource *fail_resource;
static unsigned draws, maps_program, maps_constants, maps_ub;
static HRESULT map_resource(ID3D11DeviceContext *,ID3D11Resource *,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE *);
static void draw_indexed(ID3D11DeviceContext *c,UINT n,UINT start,INT base)
{ draws++;c->lpVtbl->DrawIndexed(c,n,start,base); }
#undef ID3D11DeviceContext_Map
#undef ID3D11DeviceContext_DrawIndexed
#define ID3D11DeviceContext_Map map_resource
#define ID3D11DeviceContext_DrawIndexed draw_indexed
#include "../../src/video/nv2a_d3d11.c"
#include "../nv2a_test_host.h"

static uint8_t ram[0x100000];
static uint32_t instructions[136][4],program_version,constant_version;
static float constants[192][4];
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)ram; }
HWND xbox_FramebufferWindowHandle(void) { return NULL; }
int xbox_FramebufferScanSource(uint32_t *va,uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t n) {}
const uint32_t (*nv2a_vsh_program(uint32_t *start,uint32_t *version))[4]
{ *start=0;*version=program_version;return (const uint32_t (*)[4])instructions; }
const float (*nv2a_vsh_constants(uint32_t *version))[4]
{ *version=constant_version;return (const float (*)[4])constants; }
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static HRESULT map_resource(ID3D11DeviceContext *c,ID3D11Resource *r,UINT sub,D3D11_MAP type,
                            UINT flags,D3D11_MAPPED_SUBRESOURCE *m)
{
    if(r==(ID3D11Resource *)s_cb_prog) maps_program++;
    if(r==(ID3D11Resource *)s_cb_vconst) maps_constants++;
    if(r==(ID3D11Resource *)s_cb_ub) maps_ub++;
    if(r==fail_resource) {fail_resource=NULL;return E_OUTOFMEMORY;}
    return c->lpVtbl->Map(c,r,sub,type,flags,m);
}
static NvD3DState state(void)
{
    NvD3DState st={0};st.color_addr=0x20000;st.color_pitch=64*4;
    st.surface_format=0x28;st.clip_w=st.clip_h=64;st.color_mask=0x01010101;
    st.front_face=0x901;return st;
}
static void program(unsigned color_constant)
{
    memset(instructions,0,sizeof instructions);
    /* MOV oPos,v0; MOV oD0,c[color_constant]; END. Identity xyzw swizzle. */
    instructions[0][1]=(1u<<21)|0x1B;
    instructions[0][2]=2u<<26;
    instructions[0][3]=(15u<<12)|(1u<<11);
    instructions[1][1]=(1u<<21)|(color_constant<<13)|0x1B;
    instructions[1][2]=3u<<26;
    instructions[1][3]=(15u<<12)|(1u<<11)|(3u<<3)|1;
}
static void triangle(void)
{
    NvD3DState st=state();float pos[3][4]={{8,8,0,1},{56,8,0,1},{8,56,0,1}};
    NvD3DAttrib a[16]={0};float def[16][4]={{0}};uint32_t idx[3]={0,1,2};
    a[0].enabled=1;a[0].type=2;a[0].size=4;a[0].stride=16;a[0].data=(const uint8_t *)pos;
    nv2a_d3d_draw_ub(&st,NV_D3D_TRIANGLES,a,def,1,idx,3,0,2,NULL);
}
static void clear(void)
{ NvD3DState st=state();nv2a_d3d_clear(&st,0xF0,0xFF000000,0,0,0,63,63); }
static uint32_t pixel(void)
{
    Surface *s=surface_find(0x20000);ID3D11Texture2D *stage;
    D3D11_TEXTURE2D_DESC d;D3D11_MAPPED_SUBRESOURCE m;uint32_t out;
    CHECK(s);surface_resolve(s);ID3D11Texture2D_GetDesc(s->tex,&d);
    d.Usage=D3D11_USAGE_STAGING;d.BindFlags=d.MiscFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    CHECK(SUCCEEDED(ID3D11Device_CreateTexture2D(s_dev,&d,NULL,&stage)));
    ID3D11DeviceContext_CopyResource(s_ctx,(ID3D11Resource *)stage,(ID3D11Resource *)s->tex);
    CHECK(SUCCEEDED(ID3D11DeviceContext_Map(s_ctx,(ID3D11Resource *)stage,0,D3D11_MAP_READ,0,&m)));
    memcpy(&out,(uint8_t *)m.pData+16*m.RowPitch+16*4,4);
    ID3D11DeviceContext_Unmap(s_ctx,(ID3D11Resource *)stage,0);ID3D11Texture2D_Release(stage);
    return out;
}
static void first_draw(void)
{
    clear();triangle();CHECK(draws==1 && maps_program==1 && maps_constants==1 && maps_ub==1);
    CHECK(pixel()==0xFFFF0000u);
}
static void test_version_zero(void)
{
    clear();fail_resource=(ID3D11Resource *)s_cb_prog;triangle();
    CHECK(draws==0 && !fail_resource && maps_program==1 && maps_constants==0 && pixel()==0xFF000000u);
    fail_resource=(ID3D11Resource *)s_cb_vconst;triangle();
    CHECK(draws==0 && !fail_resource && maps_program==2 && maps_constants==1 && pixel()==0xFF000000u);
    fail_resource=(ID3D11Resource *)s_cb_ub;triangle();
    CHECK(draws==0 && !fail_resource && maps_program==2 && maps_constants==2 && maps_ub==1 && pixel()==0xFF000000u);
    triangle();CHECK(draws==1 && maps_program==2 && maps_constants==2 && maps_ub==2);
    CHECK(pixel()==0xFFFF0000u);
    triangle();CHECK(draws==2 && maps_program==2 && maps_constants==2 && maps_ub==3);
    CHECK(pixel()==0xFFFF0000u);
    puts("PASS: valid version-zero MOV program/constants/per-draw failed uploads all retry, produce actual red pixels and reuse successful unchanged versions");
}
static void test_program(void)
{
    first_draw();program(1);program_version=1;clear();
    UINT raw=s_raw_pos,index=s_ib_pos;fail_resource=(ID3D11Resource *)s_cb_prog;
    triangle();CHECK(!fail_resource && draws==1 && maps_program==2);
    CHECK(s_raw_pos==raw && s_ib_pos==index && pixel()==0xFF000000u);
    triangle();CHECK(draws==2 && maps_program==3 && maps_constants==1);
    CHECK(pixel()==0xFF00FF00u);
    puts("PASS: failed changed program Map skips stale draw, retains ring positions and retries same version to actual green pixels");
}
static void test_constants(void)
{
    first_draw();constant_version=1;constants[0][0]=0;constants[0][2]=1;clear();
    UINT raw=s_raw_pos,index=s_ib_pos;fail_resource=(ID3D11Resource *)s_cb_vconst;
    triangle();CHECK(!fail_resource && draws==1 && maps_constants==2 && maps_program==1);
    CHECK(s_raw_pos==raw && s_ib_pos==index && pixel()==0xFF000000u);
    triangle();CHECK(draws==2 && maps_constants==3 && maps_program==1);
    CHECK(pixel()==0xFF0000FFu);
    /* A successful new program remains valid when its following constant Map
     * fails: retry the constants without reuploading the successful program. */
    program(1);program_version=2;constant_version=2;clear();fail_resource=(ID3D11Resource *)s_cb_vconst;
    triangle();CHECK(draws==2 && maps_program==2 && maps_constants==4 && pixel()==0xFF000000u);
    triangle();CHECK(draws==3 && maps_program==2 && maps_constants==5 && pixel()==0xFF00FF00u);
    puts("PASS: failed constants retry, including after successful changed program; exact blue/green GPU pixels");
}
static void test_ub(void)
{
    first_draw();clear();UINT raw=s_raw_pos,index=s_ib_pos;
    fail_resource=(ID3D11Resource *)s_cb_ub;triangle();CHECK(!fail_resource && draws==1 && maps_ub==2);
    CHECK(s_raw_pos==raw && s_ib_pos==index && pixel()==0xFF000000u);
    triangle();CHECK(draws==2 && maps_ub==3 && maps_program==1 && maps_constants==1);
    CHECK(pixel()==0xFFFF0000u);
    puts("PASS: failed per-draw vertex constants skip stale offsets; same-data retry produces exact red pixels");
}
static void test_rings(void)
{
    first_draw();program(1);program_version=1;clear();
    s_raw_pos=VB_BYTES-16;s_ib_pos=IB_BYTES-4;fail_resource=(ID3D11Resource *)s_cb_prog;
    triangle();CHECK(draws==1 && !fail_resource && s_raw_pos==0 && s_ib_pos==0 && pixel()==0xFF000000u);
    triangle();CHECK(draws==2 && s_raw_pos==256 && s_ib_pos==256 && pixel()==0xFF00FF00u);
    clear();fail_resource=(ID3D11Resource *)s_raw;triangle();CHECK(draws==2 && !fail_resource);
    CHECK(s_raw_pos==256 && s_ib_pos==256 && pixel()==0xFF000000u);
    triangle();CHECK(draws==3 && pixel()==0xFF00FF00u);
    clear();fail_resource=(ID3D11Resource *)s_ib;UINT raw=s_raw_pos,index=s_ib_pos;
    triangle();CHECK(draws==3 && !fail_resource && s_raw_pos==raw && s_ib_pos==index && pixel()==0xFF000000u);
    triangle();CHECK(draws==4 && pixel()==0xFF00FF00u);
    puts("PASS: failed program after real DISCARD wrap, raw/index Map failures and NO_OVERWRITE retries preserve ring positions and exact pixels");
}
int main(int argc,char **argv)
{
    CHECK(argc==2);_putenv_s("RECOMP_PS_SPEC","0");_putenv_s("RECOMP_MSAA","1");
    _putenv_s("RECOMP_RENDER_SCALE","1");_putenv_s("RECOMP_FPS_LIMIT","0");
    program(0);constants[0][0]=constants[0][3]=1;constants[1][1]=constants[1][3]=1;
    CHECK(nv2a_d3d_init());
    if(!strcmp(argv[1],"version_zero")) test_version_zero();
    else if(!strcmp(argv[1],"program")) test_program();
    else if(!strcmp(argv[1],"constants")) test_constants();
    else if(!strcmp(argv[1],"ub")) test_ub();
    else if(!strcmp(argv[1],"rings")) test_rings();
    else CHECK(0);
    return 0;
}
