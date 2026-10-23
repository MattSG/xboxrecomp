/* Actual decoder, synthetic device. Trace must not add device operations. */
#include "../../src/apu/apu_mmio_hook.c"
#include <string.h>
static unsigned reads, writes;
static uint64_t written;
uint64_t mcpx_apu_mmio_read(MCPXAPUState *state, uint64_t addr, unsigned size)
{
    (void)state;(void)addr;(void)size;reads++;return 0x12345678;
}
void mcpx_apu_mmio_write(MCPXAPUState *state, uint64_t addr, uint64_t val, unsigned size)
{
    (void)state;(void)addr;(void)size;writes++;written=val;
}
int main(void)
{
    const struct { uint8_t code[8]; unsigned read,write,length; uint64_t value; } cases[] = {
        {{0x8b,0x01},1,0,2,0},
        {{0x89,0x01},0,1,2,1},
        {{0x09,0x01},1,1,2,0x12345679},
        {{0xc7,0x01,5,0,0,0},0,1,6,5},
        {{0xcc},0,0,0,0}
    };
    unsigned failures=0;
    g_apu_state=(MCPXAPUState *)(uintptr_t)1;
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        CONTEXT ctx;memset(&ctx,0,sizeof(ctx));
        ctx.Rip=(uintptr_t)cases[i].code;ctx.Rax=1;
        reads=writes=0;written=0;
        int ok=apu_hook_handle_mmio(&ctx,0,0xfe800100u,cases[i].write!=0);
        if (reads!=cases[i].read || writes!=cases[i].write ||
            ok!=(cases[i].length!=0) ||
            ctx.Rip!=(uintptr_t)cases[i].code+cases[i].length ||
            (writes && written!=cases[i].value) ||
            (i==0 && ctx.Rax!=0x12345678)) {
            fprintf(stderr,"FAIL case %u: reads=%u writes=%u\n",i,reads,writes);
            failures++;
        }
    }
    if (failures) return 1;
    puts("PASS: traced/plain reads, writes, RMW and rejected op preserve device access counts");
    return 0;
}
