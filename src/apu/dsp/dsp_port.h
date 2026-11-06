/* Standalone host support for the xemu DSP interpreter; no QEMU runtime. */
#pragma once
/* Unsupported instructions/DMA modes must not silently succeed in Release. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define trace_dsp56k_execute_instruction(gp, pc) ((void)0)
#define trace_dsp56k_execute_instruction_disasm(text) ((void)0)
#define trace_event_get_state(event) false
static inline uint32_t ldl_le_p(const void *ptr)
{
    const uint8_t *p = ptr;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void stl_le_p(void *ptr, uint32_t value)
{
    uint8_t *p = ptr;
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(value >> (8 * i));
}
