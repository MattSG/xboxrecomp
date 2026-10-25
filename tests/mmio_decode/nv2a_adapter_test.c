/* Actual NV2A hook with only the register model replaced. */
#include "../../src/nv2a/nv2a_mmio_hook.c"
static NV2AState state;
static int state_available = 1, fail_init, init_calls;
static uint8_t *last_storage;
static uint64_t next_value, written;
static unsigned reads, writes, width;
NV2AState *nv2a_get_state(void) { return state_available ? &state : NULL; }
NV2AState *nv2a_init_standalone(uint8_t *v, uint32_t vs, uint8_t *r, uint32_t rs)
{ (void)vs; (void)r; (void)rs; init_calls++; last_storage = v;
  if (fail_init) return NULL;
  state_available = 1; return &state; }
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
    ctx.Rip = (uintptr_t)ah;
    CHECK(!nv2a_hook_handle_mmio(&ctx,0,NV2A_MMIO_BASE-1,1));
    CHECK(!nv2a_hook_handle_mmio(&ctx,0,NV2A_MMIO_BASE+NV2A_MMIO_SIZE,1));
    CHECK(ctx.Rip==(uintptr_t)ah && reads==2 && writes==2);
    ctx.Rip = 1; /* Failure logging must not dereference unreadable instructions. */
    CHECK(!nv2a_hook_handle_mmio(&ctx,0,NV2A_MMIO_BASE+0x8054,0));
    CHECK(ctx.Rip==1 && reads==2 && writes==2);
    nv2a_hook_init(0);
    CHECK(init_calls == 0); /* Existing borrowed state needs no second allocation. */
    init_calls = 0; state_available = 0; fail_init = 1;
    nv2a_hook_init(0);
    CHECK(init_calls == 1 && !state_available);
    uint8_t byte; SIZE_T copied = 0;
    CHECK(!ReadProcessMemory(GetCurrentProcess(), last_storage, &byte, 1, &copied));
    fail_init = 0;
    nv2a_hook_init(0);
    CHECK(init_calls == 2 && state_available);
    nv2a_hook_init(0);
    CHECK(init_calls == 2);
    CHECK(VirtualFree(last_storage, 0, MEM_RELEASE)); /* Fixture ends its mock state. */
    return failures!=0;
}
