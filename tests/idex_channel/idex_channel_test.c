/* Synthetic imports exercise the guest-visible IDE queue without game assets. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern ptrdiff_t g_xbox_mem_offset;
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }
int main(void) {
    if (xbox_IdexChannelObject || xbox_resolve_ordinal(357)) {
        puts("FAIL: native IDE channel must not fabricate a structure from a scalar marker"); return 3;
    }
    /* A host embedding may supply its own native object. This does not marshal
     * host pointers into the separate guest-layout queue. */
    uint32_t host_owned[32]={0};
    xbox_IdexChannelObject=host_owned;
    if (xbox_resolve_ordinal(357)!=(ULONG_PTR)host_owned) return 4;
    xbox_IdexChannelObject=NULL;
    if (!xbox_ConfigureKernelData(0x00740000u)) return 5;

    uint8_t image[4096]={0};
    uint32_t base=0x10000,headers=4096,sections=0x10120;
    memcpy(image,"XBEH",4); memcpy(image+0x104,&base,4);
    memcpy(image+0x108,&headers,4); memcpy(image+0x120,&sections,4);
    if (!xbox_MemoryLayoutInit(image,sizeof image)) return 10;
    uint8_t *memory=xbox_GetMemoryBase();
    uint32_t *imports=(uint32_t *)(memory+0x10000);
    imports[0]=0x80000000u|357;
    imports[1]=0x80000000u|325;
    xbox_kernel_set_thunk_address(0x10000,2);
    xbox_IdexChannelObject=host_owned;
    xbox_kernel_init();
    if (xbox_kernel_thunk_table[0]!=(ULONG_PTR)host_owned) return 6;
    xbox_IdexChannelObject=NULL;
    xbox_kernel_shutdown();
    xbox_kernel_init();
    if (xbox_kernel_thunk_table[0]) return 7;

    xbox_kernel_bridge_init();
    uint32_t channel=imports[0],head=channel+0x28;
    uint32_t forward=*(uint32_t *)(memory+head),back=*(uint32_t *)(memory+head+4);
    if(forward!=head || back!=head) {
        fprintf(stderr,"FAIL: IDE queue head=%08X links=%08X/%08X\n",head,forward,back); return 1;
    }
    if(channel<imports[1]+16 && channel+0x200>imports[1]) {
        puts("FAIL: IDE storage overlaps signature key"); return 2;
    }
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    puts("PASS: exported IDE channel has an empty circular queue in independent storage");
    return 0;
}
