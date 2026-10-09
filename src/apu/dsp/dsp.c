/*
 * MCPX DSP emulator, interpreter-only adaptation of xemu dsp.c/dsp_c.c.
 * Copyright (c) 2015 espes
 * Copyright (c) 2020-2025 Matt Borgerson
 * Adapted from Hatari DSP M56001 emulation
 * (C) 2001-2008 ARAnyM developer team
 * Adaption to Hatari (C) 2008 by Thomas Huth
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * This program is distributed WITHOUT ANY WARRANTY; without even the
 * implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See COPYING.GPL-2 for the full license.
 */
#include "dsp.h"
#include "dsp_dma_regs.h"
#include "dsp_port.h"

#define INTERRUPT_START_FRAME (1u << 1)
#define INTERRUPT_DMA_EOL (1u << 7)

/* Opt-in replay corpus, one complete frame per processor. The state is a
 * local-build ABI artifact, not a portable save-state format. */
typedef struct FrameCapture {
    DSPState *dsp;
    FILE *file;
    void *opaque;
    dsp_scratch_rw_func scratch;
    dsp_fifo_rw_func fifo;
    bool attempted;
    bool clipped;
    unsigned long frames;
} FrameCapture;
static FrameCapture captures[2];

static void capture_state(FILE *file, DSPState *dsp)
{
    DSPState state = *dsp;
    memset((void *)state.core.pram_opcache, 0, sizeof(state.core.pram_opcache));
    state.core.opaque = NULL;
    state.core.read_peripheral = NULL;
    state.core.write_peripheral = NULL;
    state.dma.mem_opaque = state.dma.rw_opaque = NULL;
    state.dma.mem_read = NULL; state.dma.mem_write = NULL;
    state.dma.scratch_rw = NULL; state.dma.fifo_rw = NULL;
    if (fwrite(&state, sizeof(state), 1, file) != 1)
        fprintf(stderr, "[DSP] Frame capture state write failed\n");
}
static void capture_transfer(FrameCapture *c, unsigned kind, uint8_t *ptr,
                             uint32_t address, size_t len, bool dir)
{
    if (kind == 1) c->scratch(c->opaque, ptr, address, len, dir);
    else c->fifo(c->opaque, ptr, address, len, dir);
    if (!c->dsp->is_gp && kind == 2 && address == 0 && dir && !c->dsp->dma.error)
        for (size_t i = 0; i + 1 < len; i += 2)
            if ((ptr[i] == 0xff && ptr[i + 1] == 0x7f) ||
                (ptr[i] == 0 && ptr[i + 1] == 0x80)) c->clipped = true;
    uint32_t event[] = {kind, address, (uint32_t)len, dir, c->dsp->dma.error};
    if (fwrite(event, sizeof(event), 1, c->file) != 1 ||
        (!event[4] && fwrite(ptr, 1, len, c->file) != len))
        fprintf(stderr, "[DSP] Frame capture transfer write failed\n");
}
static void capture_scratch(void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    capture_transfer(opaque, 1, ptr, addr, len, dir);
}
static void capture_fifo(void *opaque, uint8_t *ptr, unsigned index, size_t len, bool dir)
{
    capture_transfer(opaque, 2, ptr, index, len, dir);
}
static void capture_begin(DSPState *dsp)
{
    FrameCapture *c = &captures[dsp->is_gp ? 0 : 1];
    if (c->attempted) return;
    const char *prefix = getenv("RECOMP_DSP_FRAME_DUMP");
    if (!prefix || !*prefix) { c->attempted = true; return; }
    const char *index = getenv("RECOMP_DSP_FRAME_INDEX");
    unsigned long target = index ? strtoul(index, NULL, 10) : 0;
    if (c->frames++ < target) return;
    c->attempted = true;
    c->clipped = false;
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s.%s.frame.bin", prefix, dsp->is_gp ? "gp" : "ep");
    if (length < 0 || length >= sizeof(path) || !(c->file = fopen(path, "wb"))) {
        fprintf(stderr, "[DSP] Cannot open frame capture\n");
        return;
    }
    uint32_t header[] = {0x44535046, DSP_FRAME_CAPTURE_VERSION, sizeof(DSPState)};
    fwrite(header, sizeof(header), 1, c->file);
    capture_state(c->file, dsp);
    c->dsp = dsp; c->opaque = dsp->dma.rw_opaque;
    c->scratch = dsp->dma.scratch_rw; c->fifo = dsp->dma.fifo_rw;
    dsp->dma.rw_opaque = c;
    dsp->dma.scratch_rw = capture_scratch; dsp->dma.fifo_rw = capture_fifo;
}
static void capture_end(DSPState *dsp)
{
    FrameCapture *c = &captures[dsp->is_gp ? 0 : 1];
    if (!c->file || c->dsp != dsp) return;
    uint32_t end[5] = {0};
    fwrite(end, sizeof(end), 1, c->file);
    capture_state(c->file, dsp);
    if (fclose(c->file)) fprintf(stderr, "[DSP] Frame capture close failed\n");
    c->file = NULL;
    dsp->dma.rw_opaque = c->opaque;
    dsp->dma.scratch_rw = c->scratch; dsp->dma.fifo_rw = c->fifo;
    /* Diagnostic selection only: retain a complete EP frame with full-scale
     * PCM, rewriting earlier candidate files without changing guest execution. */
    if (!dsp->is_gp && getenv("RECOMP_DSP_CAPTURE_CLIPPED") && !c->clipped)
        c->attempted = false;
}

/* Reset interrupts a capture; omit the completion trailer so replay rejects it. */
static void capture_abort(DSPState *dsp)
{
    FrameCapture *c = &captures[dsp->is_gp ? 0 : 1];
    if (!c->file || c->dsp != dsp) return;
    if (fclose(c->file)) fprintf(stderr, "[DSP] Aborted capture close failed\n");
    c->file = NULL;
    dsp->dma.rw_opaque = c->opaque;
    dsp->dma.scratch_rw = c->scratch;
    dsp->dma.fifo_rw = c->fifo;
    fprintf(stderr, "[DSP] Frame capture interrupted by reset\n");
}

static uint32_t peripheral_read(dsp_core_t *core, uint32_t address)
{
    DSPState *dsp = core->opaque;
    switch (address) {
    case 0xffffb3: return 0; /* xemu's cycle-counter peripheral model */
    case 0xffffc5: return dsp->interrupts | (dsp->dma.eol ? INTERRUPT_DMA_EOL : 0);
    case 0xffffd4: return dsp_dma_read(&dsp->dma, DMA_NEXT_BLOCK);
    case 0xffffd5: return dsp_dma_read(&dsp->dma, DMA_START_BLOCK);
    case 0xffffd6: return dsp_dma_read(&dsp->dma, DMA_CONTROL);
    case 0xffffd7: return dsp_dma_read(&dsp->dma, DMA_CONFIGURATION);
    default: return 0xababa; /* retain upstream unmapped peripheral value */
    }
}

static void reset_peripherals(DSPState *dsp)
{
    dsp->interrupts = 0;
    dsp->dma.configuration = dsp->dma.control = 0;
    dsp->dma.start_block = dsp->dma.next_block = 0;
    dsp->dma.error = dsp->dma.eol = false;
}

static void peripheral_write(dsp_core_t *core, uint32_t address, uint32_t value)
{
    DSPState *dsp = core->opaque;
    switch (address) {
    case DSP_PERIPHERAL_RESET: reset_peripherals(dsp); break;
    case 0xffffc4:
        if (value & 1) core->is_idle = true;
        break;
    case 0xffffc5:
        dsp->interrupts &= ~value;
        if (value & INTERRUPT_DMA_EOL) dsp->dma.eol = false;
        break;
    case 0xffffd4: dsp_dma_write(&dsp->dma, DMA_NEXT_BLOCK, value); break;
    case 0xffffd5: dsp_dma_write(&dsp->dma, DMA_START_BLOCK, value); break;
    case 0xffffd6: dsp_dma_write(&dsp->dma, DMA_CONTROL, value); break;
    case 0xffffd7: dsp_dma_write(&dsp->dma, DMA_CONFIGURATION, value); break;
    }
}

static uint32_t dma_memory_read(void *opaque, int space, uint32_t addr)
{
    return dsp56k_read_memory(opaque, space, addr);
}
static void dma_memory_write(void *opaque, int space, uint32_t addr, uint32_t value)
{
    dsp56k_write_memory(opaque, space, addr, value);
}

DSPState *dsp_init(void *opaque, dsp_scratch_rw_func scratch_rw,
                   dsp_fifo_rw_func fifo_rw, bool is_gp)
{
    DSPState *dsp = calloc(1, sizeof(*dsp));
    if (!dsp) return NULL;
    dsp->is_gp = dsp->core.is_gp = is_gp;
    dsp->core.opaque = dsp;
    dsp->core.read_peripheral = peripheral_read;
    dsp->core.write_peripheral = peripheral_write;
    dsp->dma.mem_opaque = &dsp->core;
    dsp->dma.mem_read = dma_memory_read;
    dsp->dma.mem_write = dma_memory_write;
    dsp->dma.rw_opaque = opaque;
    dsp->dma.scratch_rw = scratch_rw;
    dsp->dma.fifo_rw = fifo_rw;
    memset(dsp->core.pram, 0xca, sizeof(dsp->core.pram));
    memset(dsp->core.xram, 0xca, sizeof(dsp->core.xram));
    memset(dsp->core.yram, 0xca, sizeof(dsp->core.yram));
    dsp_reset(dsp);
    return dsp;
}

void dsp_reset(DSPState *dsp)
{
    capture_abort(dsp);
    dsp56k_reset_cpu(&dsp->core);
    memset((void *)dsp->core.pram_opcache, 0, sizeof(dsp->core.pram_opcache));
    dsp->core.is_idle = false;
    dsp->core.cycle_count = 0;
    dsp->save_cycles = 0;
    reset_peripherals(dsp);
}

void dsp_bootstrap(DSPState *dsp)
{
    dsp->dma.scratch_rw(dsp->dma.rw_opaque, (uint8_t *)dsp->core.pram,
                        0, 0x800 * 4, false);
    if (dsp->dma.error) {
        dsp->core.bootstrap_failed = true;
        dsp->core.is_idle = true;
        fprintf(stderr, "[DSP] %s bootstrap transfer failed; reset required\n", dsp->is_gp ? "GP" : "EP");
        return;
    }
    for (unsigned i = 0; i < 0x800; i++) dsp->core.pram[i] &= 0xffffff;
    memset((void *)dsp->core.pram_opcache, 0, sizeof(dsp->core.pram_opcache));
    const char *prefix = getenv("RECOMP_DSP_BOOT_DUMP");
    if (prefix && *prefix) {
        char path[1024];
        int length = snprintf(path, sizeof(path), "%s.%s.p.bin", prefix, dsp->is_gp ? "gp" : "ep");
        FILE *output = length >= 0 && length < sizeof(path) ? fopen(path, "wb") : NULL;
        bool saved = false;
        if (output) {
            saved = fwrite(dsp->core.pram, 4, 0x800, output) == 0x800;
            if (fclose(output)) saved = false;
        }
        fprintf(stderr, "[DSP] %s bootstrap dump %s\n", dsp->is_gp ? "GP" : "EP", saved ? "saved" : "failed");
    }
}

void dsp_start_frame(DSPState *dsp)
{
    dsp->interrupts |= INTERRUPT_START_FRAME;
    if (!dsp->core.is_waiting && !dsp->core.is_stopped && !dsp->core.is_debugging && !dsp->core.bootstrap_failed) dsp->core.is_idle = false;
    dsp->core.cycle_count = 0;
    capture_begin(dsp);
}

bool dsp_has_work(const DSPState *dsp)
{
    if (dsp->core.bootstrap_failed) return false;
    return !dsp->core.is_idle ||
           (dsp->core.is_waiting && (dsp->dma.control & DMA_CONTROL_RUNNING) &&
            !(dsp->dma.control & DMA_CONTROL_FROZEN) && !dsp->dma.error);
}

void dsp_run(DSPState *dsp, int cycles)
{
    static int trace = -1;
    if (trace < 0) trace = getenv("RECOMP_APU_TRACE") != NULL;
    uint64_t previous_cycles = dsp->core.cycle_count;
    dsp->save_cycles += cycles;
    while (dsp->save_cycles > 0 && dsp_has_work(dsp)) {
        if (dsp->core.is_waiting) {
            dsp_dma_step(&dsp->dma);
            /* ponytail: one descriptor per two-cycle scheduler slot, matching
             * instruction-boundary granularity; refine when bus timing is known. */
            dsp->save_cycles -= 2;
            dsp->core.cycle_count += 2;
            continue;
        }
        bool dma_running = (dsp->dma.control & DMA_CONTROL_RUNNING) != 0;
        dsp56k_execute_instruction(&dsp->core);
        /* A START issued by this instruction schedules work for the next
         * instruction boundary, so the guest can observe the running engine. */
        if (dma_running && !dsp->core.is_stopped && !dsp->core.is_debugging) dsp_dma_step(&dsp->dma);
        dsp->save_cycles -= dsp->core.instr_cycle;
        dsp->core.cycle_count += dsp->core.instr_cycle;
    }
    /* A frame halt discards unused scheduling credit, not guest state. */
    if (!dsp_has_work(dsp)) {
        dsp->save_cycles = 0;
        capture_end(dsp);
    }
    if (trace && previous_cycles < 1000000 && dsp->core.cycle_count >= 1000000)
        fprintf(stderr, "[DSP] %s frame exceeded 1000000 cycles PC=%06X opcode=%06X interrupts=%06X DMA.control=%06X DMA.error=%d\n",
                dsp->is_gp ? "GP" : "EP", dsp->core.pc,
                dsp_read_memory(dsp, 'P', dsp->core.pc), dsp->interrupts,
                dsp->dma.control, dsp->dma.error);
}

static int memory_space(char space)
{
    switch (space) {
    case 'X': return DSP_SPACE_X;
    case 'Y': return DSP_SPACE_Y;
    case 'P': return DSP_SPACE_P;
    default: assert(!"Invalid DSP memory space"); return DSP_SPACE_X;
    }
}
uint32_t dsp_read_memory(DSPState *dsp, char space, uint32_t address)
{
    return dsp56k_read_memory(&dsp->core, memory_space(space), address);
}
void dsp_write_memory(DSPState *dsp, char space, uint32_t address, uint32_t value)
{
    dsp56k_write_memory(&dsp->core, memory_space(space), address, value);
}
