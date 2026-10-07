#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include <d3d/d3d8_xbox.h>
#include "xbox_memory_layout.h"
#include "nv2a_mmio_hook.h"
#include "nv2a_state.h"
#include "ohci.h"
#include "xbox_host.h"

/* The trapped APU span, matching what MemoryLayoutInit unmaps: the APU's own
 * 512 KB, not AC'97 above it. */
#define APU_TRAP_BASE 0xFE800000u
#define APU_TRAP_END  0xFE880000u
#define PVIDEO_PAGE   0xFD008000u

extern ptrdiff_t g_xbox_mem_offset;

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *g_apu_state;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern void mcpx_apu_shutdown(MCPXAPUState *apu);
extern bool apu_hook_handle_mmio(PCONTEXT context, uintptr_t fault_address,
                                 uint32_t guest_address, int is_write);

static PVOID s_mmio_veh;
static IDirect3D8 *s_d3d;
static IDirect3DDevice8 *s_device;

static LONG CALLBACK mmio_handler(PEXCEPTION_POINTERS info)
{
    uintptr_t fault_address;
    uint32_t guest_address;
    int is_write;

    if (!info ||
        info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;

    fault_address = info->ExceptionRecord->ExceptionInformation[1];
    guest_address = (uint32_t)(fault_address - (uintptr_t)g_xbox_mem_offset);
    is_write = info->ExceptionRecord->ExceptionInformation[0] ? 1 : 0;
    if (xbox_OhciOwnsAddress(guest_address))
        return xbox_OhciHandleMmio(info->ContextRecord, guest_address)
            ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    if (guest_address == PVIDEO_PAGE + 0x700u && !is_write &&
        !getenv("RECOMP_FB_WINDOW")) {
        NV2AState *gpu = nv2a_get_state();
        if (gpu) {
            /* ponytail: without a scanout window, consume rather than stall. */
            uint64_t buffer = nv2a_mmio_read(gpu, 0x8700, 4);
            nv2a_mmio_write(gpu, 0x8700, buffer & ~0x11u, 4);
        }
    }
    if (guest_address >= PVIDEO_PAGE && guest_address < PVIDEO_PAGE + 0x1000u)
        return nv2a_hook_handle_mmio(info->ContextRecord, fault_address,
                                     guest_address, is_write)
            ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    if (g_apu_state &&
        guest_address >= APU_TRAP_BASE && guest_address < APU_TRAP_END)
        return apu_hook_handle_mmio(info->ContextRecord, fault_address,
                                    guest_address, is_write)
            ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_SEARCH;
}

int xbox_HostHardwareInit(void)
{
    if (getenv("RECOMP_AC97_READY")) {
        g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
        if (!g_apu_state) {
            fprintf(stderr, "[APU] initialization failed\n");
            return 0;
        }
    }
    s_mmio_veh = AddVectoredExceptionHandler(1, mmio_handler);
    if (!s_mmio_veh) {
        fprintf(stderr, "[HOST] could not install MMIO exception handler\n");
        xbox_HostHardwareShutdown();
        return 0;
    }
    /* XAPI is linked into the title and drives the controllers' registers
     * itself; without them it reads zeroed MCPX RAM and never finds a pad. */
    xbox_OhciInit();
    fprintf(stderr, "[HOST] MMIO routed (APU %s, USB)\n",
            g_apu_state ? "up" : "off");
    return 1;
}

void xbox_HostHardwareShutdown(void)
{
    if (s_mmio_veh) {
        RemoveVectoredExceptionHandler(s_mmio_veh);
        s_mmio_veh = NULL;
    }
    if (g_apu_state) {
        mcpx_apu_shutdown(g_apu_state);
        g_apu_state = NULL;
    }
}

static LRESULT CALLBACK graphics_wndproc(HWND window, UINT message,
                                         WPARAM wparam, LPARAM lparam)
{
    if (message == WM_CLOSE || message == WM_DESTROY)
        return 0;
    return DefWindowProcA(window, message, wparam, lparam);
}

int xbox_HostGraphicsInit(const char *window_title)
{
    WNDCLASSA window_class;
    D3DPRESENT_PARAMETERS present;
    HWND window;
    HRESULT hr;
    DWORD old_protect;

    memset(&window_class, 0, sizeof(window_class));
    window_class.lpfnWndProc = graphics_wndproc;
    window_class.hInstance = GetModuleHandleA(NULL);
    window_class.lpszClassName = "XboxRecompGraphics";
    RegisterClassA(&window_class);

    window = CreateWindowExA(0, window_class.lpszClassName, window_title,
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             640, 480, NULL, NULL, window_class.hInstance, NULL);
    if (!window) {
        fprintf(stderr, "graphics: window creation failed\n");
        return 0;
    }

    s_d3d = xbox_Direct3DCreate8(0);
    if (!s_d3d) {
        fprintf(stderr, "graphics: Direct3DCreate8 failed\n");
        return 0;
    }

    memset(&present, 0, sizeof(present));
    present.BackBufferWidth = 640;
    present.BackBufferHeight = 480;
    present.BackBufferFormat = D3DFMT_X8R8G8B8;
    present.BackBufferCount = 1;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.hDeviceWindow = window;
    present.Windowed = TRUE;
    present.EnableAutoDepthStencil = TRUE;
    present.AutoDepthStencilFormat = D3DFMT_D24S8;

    hr = s_d3d->lpVtbl->CreateDevice(s_d3d, 0, 0, window, 0, &present, &s_device);
    if (FAILED(hr) || !s_device) {
        fprintf(stderr, "graphics: CreateDevice failed: 0x%08lX\n",
                (unsigned long)hr);
        return 0;
    }
    fprintf(stderr, "graphics: D3D8/D3D11 device initialized\n");

    nv2a_hook_init(g_xbox_mem_offset);
    /* PVIDEO accesses must execute register semantics, including STOP. */
    if (!VirtualProtect((void *)((uintptr_t)g_xbox_mem_offset + PVIDEO_PAGE),
                        0x1000, PAGE_NOACCESS, &old_protect)) {
        fprintf(stderr, "[NV2A] could not trap PVIDEO page\n");
        return 0;
    }
    return 1;
}
