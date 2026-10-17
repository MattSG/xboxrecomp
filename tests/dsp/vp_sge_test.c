/* get_data_ptr from the real apu_vp.c: a page index past max_sge is refused
 * (false, out untouched) instead of reading the SGE table past its bound. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/apu/apu_vp.c"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "VP SGE check line %d: %s\n", __LINE__, #condition); \
    exit(1); } } while (0)

struct McpxApuDebug g_dbg, g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4];
static uint8_t ram[0x4000];
uint8_t *g_apu_ram_ptr = ram;
uint8_t *mcpx_apu_phys(uint64_t address)
{
    CHECK(address < sizeof(ram)); /* guest reads only: SGE table and data */
    return ram + address;
}

int main(void)
{
    hwaddr out = 0xdead;

    /* SGE table at 0x1000 with 8-byte entries; entry 1 maps to page 0x2000. */
    stl_le_phys(address_space_memory, 0x1008, 0x2000);
    CHECK(get_data_ptr(0x1000, 1, 0x1234, &out));
    CHECK(out == 0x2000 + 0x234);

    /* Entry 2 is past max_sge 1: refused, and the table is not consulted. */
    out = 0xdead;
    CHECK(!get_data_ptr(0x1000, 1, 0x2000, &out));
    CHECK(out == 0xdead);

    /* The VP has no SGE length register, so its callers bound the index by the
     * largest page a 32-bit linear address reaches. */
    CHECK(VP_SGE_MAX_ENTRY == 0xFFFFFu);
    printf("VP SGE: page index past max refused; in-range lookup resolves\n");
    return 0;
}
