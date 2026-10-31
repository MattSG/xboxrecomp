/* Exercise the actual pump with synthetic decoder/device operations.
 * No real decoding, rendering or window is claimed by this contract check. */
#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "video/video_player.h"
#include "d3d/d3d8_xbox.h"

static HWND WINAPI fake_window(DWORD a, LPCSTR b, LPCSTR c, DWORD d,
    int e, int f, int g, int h, HWND i, HMENU j, HINSTANCE k, LPVOID l)
{
    (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;
    (void)i;(void)j;(void)k;(void)l;
    return (HWND)(uintptr_t)1;
}
#define CreateWindowExA fake_window
#include "../../src/video/video_pump.c"
#undef CreateWindowExA

static unsigned presented, updates, limit, callbacks, dumps, failures;
static HRESULT __stdcall fake_scene(IDirect3DDevice8 *self) { (void)self; return S_OK; }
static HRESULT __stdcall fake_clear(IDirect3DDevice8 *self, DWORD n,
    const D3DRECT *r, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    (void)self;(void)n;(void)r;(void)flags;(void)color;(void)z;(void)stencil;
    return S_OK;
}
static HRESULT __stdcall fake_present(IDirect3DDevice8 *self,
    const RECT *a, const RECT *b, HWND c, void *d)
{
    (void)self;(void)a;(void)b;(void)c;(void)d;presented++;return S_OK;
}
static const IDirect3DDevice8Vtbl device_methods = {
    .Present=fake_present, .Clear=fake_clear, .BeginScene=fake_scene, .EndScene=fake_scene
};
static IDirect3DDevice8 device = { &device_methods };
static HRESULT __stdcall fake_device(IDirect3D8 *self, UINT adapter,
    DWORD type, HWND window, DWORD flags, D3DPRESENT_PARAMETERS *pp,
    IDirect3DDevice8 **out)
{
    (void)self;(void)adapter;(void)type;(void)window;(void)flags;(void)pp;
    *out=&device; return S_OK;
}
static const IDirect3D8Vtbl factory_methods = { .CreateDevice=fake_device };
static IDirect3D8 factory = { &factory_methods };
IDirect3D8 *xbox_Direct3DCreate8(UINT version) { (void)version;return &factory; }
int video_init(void) { return 0; }
void video_shutdown(void) {}
int video_open(const char *path) { (void)path;updates=0;return 0; }
int video_update(float dt) { (void)dt;return ++updates>limit ? -1 : 1; }
int video_is_finished(void) { return 0; }
void video_render(void) {}
void video_close(void) {}
int video_dump_frame_bmp(const char *path) { (void)path;dumps++;return 0; }
static void on_present(unsigned frame)
{
    callbacks++;
    if (frame != callbacks || frame != presented) failures++;
    if (frame==3 || frame==5) video_dump_frame_bmp("caller-selected");
}
static void run(unsigned frames)
{
    presented=callbacks=dumps=0;limit=frames;s_playing=1;
    strcpy_s(s_path,sizeof(s_path),"synthetic");
    vp_thread(NULL);
    if (presented!=frames || xbox_VideoIsPlaying()) failures++;
}
int main(void)
{
    /* Legacy environment alone must no longer select frames 60/80. */
    _putenv_s("RECOMP_FMV_DUMP","must-not-be-written");
    run(82);if (callbacks || dumps) failures++;
    xbox_VideoSetPresentHandler(on_present);
    run(7);if (callbacks!=7 || dumps!=2) failures++;
    xbox_VideoSetPresentHandler(NULL);
    run(5);if (callbacks || dumps) failures++;
    if (failures) { fprintf(stderr,"FAIL: %u\n",failures);return 1; }
    puts("PASS: default silence, present ordering/counts, caller capture selection, clear");
    return 0;
}
