/* Interpreter-only MCPX DSP state. See PROVENANCE.md and per-source licenses. */
#pragma once
#include "interp/dsp_cpu.h"
#include "dsp_dma.h"

#define DSP_FRAME_CAPTURE_VERSION 3u

typedef struct DSPState {
    dsp_core_t core;
    DSPDMAState dma;
    int save_cycles;
    uint32_t interrupts;
    bool is_gp;
} DSPState;

DSPState *dsp_init(void *opaque, dsp_scratch_rw_func scratch_rw,
                   dsp_fifo_rw_func fifo_rw, bool is_gp);
void dsp_reset(DSPState *dsp);
void dsp_bootstrap(DSPState *dsp);
void dsp_start_frame(DSPState *dsp);
bool dsp_has_work(const DSPState *dsp);
void dsp_run(DSPState *dsp, int cycles);
uint32_t dsp_read_memory(DSPState *dsp, char space, uint32_t address);
void dsp_write_memory(DSPState *dsp, char space, uint32_t address, uint32_t value);
