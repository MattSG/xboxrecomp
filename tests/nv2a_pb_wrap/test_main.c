/* The pushbuffer walk addresses guest memory as the NV2A does: through a
 * 26-bit physical bus, so targets and reads wrap at 64 MB.
 *
 * nv2a_pb_scan.c is compiled into this file; the segment walk (the default,
 * without RECOMP_PB_WALK=dma) is what runs. The contiguous window is 64 MB
 * of committed memory followed by 64 MB reserved and inaccessible, so a read
 * past the window faults instead of passing unnoticed. Both checks fault on
 * main, which masked to 256 MB:
 *   - a CALL whose target is above 64 MB reaches the subroutine below it;
 *   - a method whose parameters run off the top of RAM reads them from the
 *     bottom.
 *
 * Run with: ctest --test-dir <build> */
#include "../../src/kernel/nv2a_pb_scan.c"

#include <windows.h>
#include "kernel.h"   /* XBOX_CONTIG_BASE, XBOX_CONTIG_SIZE */

static uint8_t *s_window;               /* host address of physical 0 */
static uint32_t s_seen_param[8];
static int s_seen_n;
static int s_failures;

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return (ptrdiff_t)((uintptr_t)s_window - XBOX_CONTIG_BASE);
}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    (void)subch; (void)method;
    if (s_seen_n < 8)
        s_seen_param[s_seen_n++] = param;
}

void nv2a_pb_exec_report(void) {}
void xbox_Nv2aAckBusyBits(void) {}
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset) { (void)offset; return NULL; }

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); s_failures++; } } while (0)

static void put(uint32_t physical, uint32_t word)
{
    memcpy(s_window + physical, &word, 4);
}

/* A method header: count parameters for method on subchannel 0. */
static uint32_t method(uint32_t m, uint32_t count) { return (count << 18) | m; }

static void call_above_64mb_wraps(void)
{
    s_seen_n = 0;
    put(0x1000, 0x04000100u | 2u);           /* CALL 64 MB + 0x100 */
    put(0x0100, method(0x0100, 1));          /* the subroutine at 0x100 */
    put(0x0104, 0xC0FFEEu);
    put(0x0108, 0x00020000u);                /* RETURN */

    nv2a_pb_scan_segment(XBOX_CONTIG_BASE + 0x1000, XBOX_CONTIG_BASE + 0x1004);
    CHECK(s_seen_n == 1 && s_seen_param[0] == 0xC0FFEEu,
          "the CALL did not reach the subroutine at 0x100 (%d methods)", s_seen_n);
}

static void read_past_top_wraps(void)
{
    const uint32_t top = XBOX_CONTIG_SIZE - 4;

    s_seen_n = 0;
    put(top, method(0x0100, 2));             /* last word of RAM */
    put(0x0000, 0x11111111u);                /* its parameters, wrapped */
    put(0x0004, 0x22222222u);

    nv2a_pb_scan_segment(XBOX_CONTIG_BASE + top, XBOX_CONTIG_BASE + top + 12);
    CHECK(s_seen_n == 2 && s_seen_param[0] == 0x11111111u &&
          s_seen_param[1] == 0x22222222u,
          "parameters past the top of RAM were not read from the bottom "
          "(%d methods)", s_seen_n);
}

int main(void)
{
    s_window = (uint8_t *)VirtualAlloc(NULL, 2 * (size_t)XBOX_CONTIG_SIZE,
                                       MEM_RESERVE, PAGE_NOACCESS);
    if (!s_window || !VirtualAlloc(s_window, XBOX_CONTIG_SIZE, MEM_COMMIT,
                                   PAGE_READWRITE)) {
        fprintf(stderr, "could not map the test window\n");
        return 2;
    }
    s_exec_enabled = 1;

    call_above_64mb_wraps();
    read_past_top_wraps();

    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    puts("PASS: pushbuffer targets and reads wrap at 64 MB.");
    return 0;
}
