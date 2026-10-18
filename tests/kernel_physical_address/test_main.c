/* Unmapped guest VAs have no physical address; real alias translation is in
 * kernel_io_mapping, which initializes the backing sections. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef void (*guest_fn)(void);
extern guest_fn recomp_lookup_kernel(uint32_t);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }
int main(void) {
    if (xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)0x80001000u) != 0) {
        fputs("FAIL uninitialized contiguous VA must be unsupported\n", stderr);
        return 12;
    }
    /* Exercise boundaries around the mapped base, contiguous window and the
     * first address beyond it. This test has no physical backing, so all are
     * unsupported; kernel_io_mapping covers backed RAM aliases and bridge ABI. */
    const uint32_t cases[]={
        0,0x1000,0x10000,0x03ffffff,0x7fffffff,
        0x80000000,0x80001000,0x83ffffff,0x84000000,0xffffffff
    };
    uint8_t *memory=VirtualAlloc(NULL,16*1024*1024,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    if(!memory) return 10;
    g_xbox_mem_offset=(ptrdiff_t)memory;
    *(uint32_t *)(memory+0x10000)=0x800000AD;
    xbox_kernel_set_thunk_address(0x10000,1); xbox_kernel_bridge_init();
    int failed=0;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        uint32_t *stack=(uint32_t *)(memory+0x20000);
        stack[0]=0x12345678; stack[1]=cases[i]; stack[2]=0xABCDEF01;
        g_esp=0x20000;
        guest_fn fn=recomp_lookup_kernel(*(uint32_t *)(memory+0x10000));
        if(!fn) return 11;
        fn();
        if(g_eax!=0 || g_esp!=0x20008 || stack[2]!=0xABCDEF01) {
            fprintf(stderr,"FAIL unmapped guest VA=%08X expected=0 actual=%08X ESP=%08X\n",
                    cases[i],g_eax,g_esp); failed=1;
        }
    }
    VirtualFree(memory,0,MEM_RELEASE);
    if(!failed) puts("PASS ten unmapped-address boundaries and stdcall stack cleanup");
    return failed;
}
