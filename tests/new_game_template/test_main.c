/* Real starter control flow, with runtime boundaries mocked. */
#include <xbox/xboxrecomp.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static int WINAPI fixture_message(HWND w,LPCSTR text,LPCSTR title,UINT type)
{ (void)w; (void)text; (void)title; (void)type; return IDOK; }
#define MessageBoxA fixture_message
#define WinMain starter_main
#define main starter_console_main
#include "../../templates/new-game/src/main.c"
#undef main
#undef WinMain
#undef MessageBoxA

RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi,g_seh_ebp;
ptrdiff_t g_xbox_mem_offset;
uint32_t g_xbox_code_lo,g_xbox_code_hi;
volatile uint32_t g_icall_trace[16],g_icall_trace_idx;
static const char *mode;
static char order[32];
static unsigned calls;
static void note(char c) { assert(calls+1<sizeof order); order[calls++]=c; order[calls]=0; }
BOOL xbox_MemoryLayoutInit(const void *data,size_t size)
{ assert(data && size); note('M'); return strcmp(mode,"memory-fail")!=0; }
void xbox_MemoryLayoutShutdown(void) { note('m'); }
ptrdiff_t xbox_GetMemoryOffset(void) { return 0x10000000; }
void xbox_kernel_init(void) { note('K'); }
void xbox_kernel_shutdown(void) { note('k'); }
void xbox_path_init(const char *dir,const char *save) { assert(dir && !save); note('P'); }
void xbox_kernel_bridge_init(void) { note('B'); }
void xbox_SetThreadMode(int value) { assert(value==XBOX_THREAD_MODE_INLINE); note('I'); }
int xbox_HostHardwareInit(void) { note('H'); return strcmp(mode,"hardware-fail")!=0; }
int xbox_HostGraphicsInit(const char *title)
{ assert(!strcmp(title,"YOUR_GAME_NAME")); note('G'); return strcmp(mode,"graphics-fail")!=0; }
void xbox_HostHardwareShutdown(void) { note('h'); }
int recomp_dispatch_init(void) { note('D'); return 1; }
void xbox_WatchdogStart(void) { note('W'); }
void xbe_entry_point(void) { assert(g_esp==XBOX_STACK_TOP); note('E'); }
int main(int argc,char **argv)
{
    assert(argc==2); mode=argv[1];
    int result=starter_main(NULL,NULL,NULL,0);
    const char *expected=!strcmp(mode,"success") ? "MKPBIHGDWEhkm" :
        !strcmp(mode,"memory-fail") ? "M" :
        !strcmp(mode,"hardware-fail") ? "MKPBIHhkm" : "MKPBIHGhkm";
    assert(result==(!strcmp(mode,"success") ? 0 : 1));
    if(strcmp(order,expected)) fprintf(stderr,"order=%s expected=%s\n",order,expected);
    assert(!strcmp(order,expected));
    puts("PASS: shared host bootstrap and cleanup order");
    return 0;
}
