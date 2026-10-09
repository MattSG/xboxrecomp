/* Replace only native queue allocation; compile the real DSP/FIFO glue. */
#include <stdlib.h>
extern int dsp_test_fail_alloc;
static void *test_realloc(void *ptr, size_t size)
{
    return dsp_test_fail_alloc ? NULL : realloc(ptr, size);
}
#define realloc test_realloc
#include "../../src/apu/apu_dsp.c"
