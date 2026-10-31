/* Instrument native staging operations; compile the real DSP/FIFO glue. */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
extern int dsp_test_fail_alloc;
uint64_t dsp_test_pcm_allocations, dsp_test_pcm_copied, dsp_test_pcm_moved;
static void *test_realloc(void *ptr, size_t size)
{
    dsp_test_pcm_allocations++;
    return dsp_test_fail_alloc ? NULL : realloc(ptr, size);
}
static void *test_memcpy(void *destination, const void *source, size_t size)
{
    dsp_test_pcm_copied += size;
    return memcpy(destination, source, size);
}
static void *test_memmove(void *destination, const void *source, size_t size)
{
    dsp_test_pcm_moved += size;
    return memmove(destination, source, size);
}
#define realloc test_realloc
#define memcpy test_memcpy
#define memmove test_memmove
#include "../../src/apu/apu_dsp.c"

void dsp_test_publish_pcm_frame(MCPXAPUState *d, const uint8_t *pcm)
{
    publish_pcm_frame(d, pcm);
}
