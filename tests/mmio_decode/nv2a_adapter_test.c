/* Actual NV2A hook with only the register model replaced. */
#include "../../src/nv2a/nv2a_mmio_hook.c"
static NV2AState state;
static uint64_t next_value, written;
static unsigned reads, writes, width;
NV2AState *nv2a_get_state(void) { return &state; }
NV2AState *nv2a_init_standalone(uint8_t *v, uint32_t vs, uint8_t *r, uint32_t rs)
{ (void)v; (void)vs; (void)r; (void)rs; return &state; }
uint64_t nv2a_mmio_read(NV2AState *s, hwaddr a, unsigned n)
{ (void)s; (void)a; width = n; reads++; return next_value; }
void nv2a_mmio_write(NV2AState *s, hwaddr a, uint64_t value, unsigned n)
{ (void)s; (void)a; width = n; writes++; written = value; }
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s\n", #x); failures++; } } while (0)
int main(void)
{
    CONTEXT ctx = {0};
    const uint8_t word[] = {0x66,0xC7,0,0x34,0x12,0xAA,0xBB};
    ctx.Rip = (uintptr_t)word;
    CHECK(nv2a_hook_handle_mmio(&ctx,0,NV2A_MMIO_BASE+0x8054,1));
    CHECK(width==2 && written==0x1234 && writes==1);
    CHECK(ctx.Rip==(uintptr_t)word+5);
    const uint8_t ah[] = {0x88,0x20};
    ctx.Rip=(uintptr_t)ah; ctx.Rax=0xAB00; ctx.Rsp=0x55;
    CHECK(nv2a_hook_handle_mmio(&ctx,0,NV2A_MMIO_BASE+0x8054,1));
    CHECK(written==0xAB);
    const uint8_t sib[] = {0x8B,0x0C,0x25,0,0x10,0,0};
    ctx.Rip=(uintptr_t)sib; next_value=0x12345678;
    CHECK(nv2a_hook_handle_mmio(&ctx,0,NV2A_MMIO_BASE+0x8054,0));
    CHECK(ctx.Rcx==0x12345678 && ctx.Rip==(uintptr_t)sib+sizeof sib);
    const uint8_t reverse[] = {0x3A,0x08};
    ctx.Rip=(uintptr_t)reverse; ctx.Rcx=1; next_value=0x80; ctx.EFlags=0x400;
    CHECK(nv2a_hook_handle_mmio(&ctx,0,NV2A_MMIO_BASE+0x8054,0));
    CHECK((ctx.EFlags&0xCD5)==0xC85);
    CHECK(reads==2);
    ctx.Rip = 1; /* Failure logging must not dereference unreadable instructions. */
    CHECK(!nv2a_hook_handle_mmio(&ctx,0,NV2A_MMIO_BASE+0x8054,0));
    CHECK(ctx.Rip==1 && reads==2 && writes==2);
    return failures!=0;
}
