/*
 * MCPX DSP DMA
 *
 * Copyright (c) 2015 espes
 * Copyright (c) 2020-2021 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "dsp_port.h"

#include "debug.h"
#include "dsp_dma.h"
#include "dsp_dma_regs.h"
#include "interp/dsp_cpu_regs.h"

#ifdef DEBUG

const char *buffer_names[] = {
    "fifo0",            /* 0x0 */
    "fifo1",            /* 0x1 */
    "fifo2",            /* 0x2 */
    "fifo3",            /* 0x3 */
    "<unknown-0x4>",    /* 0x4 */
    "<unknown-0x5>",    /* 0x5 */
    "<unknown-0x6>",    /* 0x6 */
    "<unknown-0x7>",    /* 0x7 */
    "<unknown-0x8>",    /* 0x8 */
    "<unknown-0x9>",    /* 0x9 */
    "<unknown-0xa>",    /* 0xA */
    "<unknown-0xb>",    /* 0xB */
    "<unknown-0xc>",    /* 0xC */
    "<unknown-0xd>",    /* 0xD */
    "scratch-circular", /* 0xE */
    "scratch"           /* 0xF */
};

const char *format_names[] = {
    "8 bit",         /* 0x0 */
    "16 bit",        /* 0x1 */
    "24 bit msb",    /* 0x2 */
    "32 bit",        /* 0x3 */
    "<invalid-0x4>", /* 0x4 */
    "<invalid-0x5>", /* 0x5 */
    "24 bit lsb",    /* 0x6 */
    "<invalid-0x7>"  /* 0x7 */
};

const char *space_names[] = {
    "x", /* DSP_SPACE_X, 0x0 */
    "y", /* DSP_SPACE_Y, 0x1 */
    "p"  /* DSP_SPACE_P, 0x2 */
};

#endif

static void scratch_circular_copy(
    DSPDMAState *s,
    uint32_t     scratch_base,
    uint32_t    *scratch_offset,
    uint32_t     scratch_size,
    uint32_t     transfer_size,
    uint8_t     *scratch_buf,
    int          direction)
{
    if (*scratch_offset >= scratch_size) {
        // fprintf(stderr, "Initial scratch offset exceeds scratch size! Wrapping\n");
        *scratch_offset = 0;
    }

    uint32_t buf_offset = 0;

    while (transfer_size > 0) {
        size_t bytes_until_wrap = scratch_size - *scratch_offset;
        size_t chunk_size = MIN(transfer_size, bytes_until_wrap);
        uint32_t scratch_addr = scratch_base + *scratch_offset;

        // R/W to scratch memory from chunk in buffer
        s->scratch_rw(s->rw_opaque, &scratch_buf[buf_offset], scratch_addr, chunk_size, direction);
        if (s->error) return;

        // Advance scratch pointer, wrap if we've reached the end
        *scratch_offset += chunk_size;
        if (*scratch_offset >= scratch_size) {
            *scratch_offset = 0;
        }

        transfer_size -= chunk_size;
        buf_offset += chunk_size;
    }
}

static bool dma_memory_range(uint32_t dsp_offset, uint32_t words, int *space, uint32_t *address)
{
    *space = DSP_SPACE_X;
    uint32_t available_words = 0;
    if (dsp_offset < DSP_XRAM_SIZE) {
        *address = dsp_offset;
        available_words = DSP_XRAM_SIZE - dsp_offset;
    } else if (dsp_offset >= DSP_MIXBUFFER_BASE && dsp_offset < DSP_MIXBUFFER_BASE + DSP_MIXBUFFER_SIZE) {
        *address = dsp_offset;
        available_words = DSP_MIXBUFFER_BASE + DSP_MIXBUFFER_SIZE - dsp_offset;
    } else if (dsp_offset >= 0x1800 && dsp_offset < 0x2000) {
        *space = DSP_SPACE_Y;
        *address = dsp_offset - 0x1800;
        available_words = DSP_YRAM_SIZE - *address;
    } else if (dsp_offset >= 0x2800 && dsp_offset < 0x3800) {
        *space = DSP_SPACE_P;
        *address = dsp_offset - 0x2800;
        available_words = DSP_PRAM_SIZE - *address;
    }
    return available_words && words <= available_words;
}

void dsp_dma_step(DSPDMAState *s)
{
    if (!(s->control & DMA_CONTROL_RUNNING)
        || (s->control & DMA_CONTROL_FROZEN) || s->error) {
        return;
    }

    /* ponytail: one descriptor per scheduler step; model bus cycles if measured
     * hardware timing requires finer transfer granularity. Reads never advance DMA. */
    if (!(s->next_block & NODE_POINTER_EOL)) {
        uint32_t addr = s->next_block & NODE_POINTER_VAL;
        uint32_t block_addr = 0;
        int block_space = DSP_SPACE_X;

        if (!dma_memory_range(addr, 7, &block_space, &block_addr)) {
            fprintf(stderr, "Unsupported DSP DMA descriptor range: %x\n", addr);
            s->error = true;
            s->control &= ~DMA_CONTROL_RUNNING;
            s->control |= DMA_CONTROL_STOPPED;
            return;
        }

        uint32_t next_block = s->mem_read(s->mem_opaque, block_space, block_addr);
        uint32_t control = s->mem_read(s->mem_opaque, block_space, block_addr+1);
        uint32_t count = s->mem_read(s->mem_opaque, block_space, block_addr+2);

        uint32_t dsp_offset = s->mem_read(s->mem_opaque, block_space, block_addr+3);
        uint32_t scratch_offset = s->mem_read(s->mem_opaque, block_space, block_addr+4);
        uint32_t scratch_base = s->mem_read(s->mem_opaque, block_space, block_addr+5);
        uint32_t scratch_size = s->mem_read(s->mem_opaque, block_space, block_addr+6)+1;

        s->next_block = next_block;

        /* Decode control word */
        bool     dsp_interleave          = (control >> 0) & 1;
        bool     direction               = control & NODE_CONTROL_DIRECTION;
        uint32_t unk2                    = (control >>  2) & 0x3;
        bool     buffer_offset_writeback = (control >>  4) & 1;
        uint32_t buf_id                  = (control >>  5) & 0xf;
        // bool     unk9                    = (control >>  9) & 1; /* FIXME: What does this do? */
        uint32_t format                  = (control >> 10) & 0x7;
        bool     unk13                   = (control >> 13) & 1;
        // uint32_t dsp_step                = (control >> 14) & 0x3FF; // FIXME

        if (buf_id != 0xe && buf_id != 0xf && buf_id >= (direction ? 4u : 2u)) {
            fprintf(stderr, "Unsupported DSP DMA buffer route: %x direction=%u\n", buf_id, direction);
            s->error = true;
            s->control &= ~DMA_CONTROL_RUNNING;
            s->control |= DMA_CONTROL_STOPPED;
            return;
        }

        /* Unmodeled flags must not silently execute with different semantics. */
        if (unk2 || unk13) {
            fprintf(stderr, "Unsupported DSP DMA descriptor control: %x\n", control);
            s->error = true;
            s->control = (s->control & ~DMA_CONTROL_RUNNING) | DMA_CONTROL_STOPPED;
            return;
        }

        /* Decode count for interleaved mode */
        uint32_t channel_count = (count & 0xF) + 1;
        uint32_t block_count = count >> 4;

        unsigned int item_size = 4;
        uint32_t item_mask = 0xffffffff;
        // bool lsb = (format == 6); // FIXME

        switch(format) {
        case 0:
            /* Packing is not implemented; never acknowledge a skipped transfer. */
            s->error = true;
            s->control &= ~DMA_CONTROL_RUNNING;
            s->control |= DMA_CONTROL_STOPPED;
            return;
        case 1:
            item_size = 2;
            item_mask = 0x0000ffff;
            break;
        case 2:
        case 6:
            item_size = 4;
            item_mask = 0x00ffffff;
            break;
        default:
            fprintf(stderr, "Unknown dsp dma format: 0x%x\n", format);
            s->error = true;
            s->control &= ~DMA_CONTROL_RUNNING;
            s->control |= DMA_CONTROL_STOPPED;
            return;
        }

        size_t scratch_addr = scratch_base + scratch_offset;
        uint32_t mem_address = 0;
        int mem_space = DSP_SPACE_X;
        uint32_t word_count = dsp_interleave ? block_count * channel_count : count;

        if (!dma_memory_range(dsp_offset, word_count, &mem_space, &mem_address)) {
            fprintf(stderr, "Unsupported DSP DMA memory range: %x words=%u\n", dsp_offset, word_count);
            s->error = true;
            s->control &= ~DMA_CONTROL_RUNNING;
            s->control |= DMA_CONTROL_STOPPED;
            return;
        }

        size_t transfer_size = (size_t)word_count * item_size;

        uint8_t *scratch_buf = malloc(transfer_size ? transfer_size : 1);
        if (!scratch_buf) {
            s->error = true;
            s->control &= ~DMA_CONTROL_RUNNING;
            s->control |= DMA_CONTROL_STOPPED;
            return;
        }

        if (direction) {
            if (dsp_interleave) {
                // Interleave samples
                for (int i = 0; i < block_count; i++) {
                    for (int ch = 0; ch < channel_count; ch++) {
                        uint32_t v = s->mem_read(s->mem_opaque,
                            mem_space, mem_address+ch*block_count+i);
                        switch(item_size) {
                        case 2:
                            *(uint16_t*)(scratch_buf + i*2*channel_count + ch*2) = v >> 8;
                            break;
                        case 4:
                            *(uint32_t*)(scratch_buf + i*4*channel_count + ch*4) = v;
                            break;
                        default:
                            assert(!"Invalid dsp dma item size for interleaved samples");
                            break;
                        }
                    }
                }
            } else {
                for (int i = 0; i < count; i++) {
                    uint32_t v = s->mem_read(s->mem_opaque, mem_space, mem_address+i);
                    switch(item_size) {
                    case 2:
                        *(uint16_t*)(scratch_buf + i*2) = v >> 8;
                        break;
                    case 4:
                        *(uint32_t*)(scratch_buf + i*4) = v;
                        break;
                    default:
                        assert(!"Invalid dsp dma item size");
                        break;
                    }
                }

            }

            /* FIXME: Move to function; then reuse for both directions */
            switch (buf_id) {
            case 0x0:
            case 0x1:
            case 0x2:
            case 0x3:
                s->fifo_rw(s->rw_opaque, scratch_buf, buf_id, transfer_size, 1);
                break;
            case 0xE:
                scratch_circular_copy(s, scratch_base, &scratch_offset, scratch_size, transfer_size, scratch_buf, 1);
                break;
            case 0xF:
                s->scratch_rw(s->rw_opaque, scratch_buf, scratch_addr, transfer_size, 1);
                break;
            default:
                fprintf(stderr, "Unknown DSP DMA buffer: 0x%x\n", buf_id);
                assert(!"Unknown dsp dma buffer");
                break;
            }
        } else {
            if (buf_id < 2) {
                s->fifo_rw(s->rw_opaque, scratch_buf, buf_id, transfer_size, 0);
            } else if (buf_id == 0xe) {
                scratch_circular_copy(s, scratch_base, &scratch_offset, scratch_size, transfer_size, scratch_buf, 0);
            } else if (buf_id == 0xf) {
                s->scratch_rw(s->rw_opaque, scratch_buf, scratch_addr, transfer_size, 0);
            } else {
                fprintf(stderr, "Unhandled DSP DMA buffer: 0x%x\n", buf_id);
                assert(!"Unhandled dsp dma buffer");
            }

            if (s->error) { free(scratch_buf); return; }

            for (uint32_t i = 0; i < word_count; i++) {
                uint32_t v;
                switch(item_size) {
                case 2:
                    v = *(uint16_t*)(scratch_buf + i*2) << 8;
                    break;
                case 4:
                    v = (*(uint32_t*)(scratch_buf + i*4)) & item_mask;
                    break;
                default:
                    v = 0;
                    assert(!"Invalid dsp dma item size");
                    break;
                }

                /* Inverse of the planar-to-interleaved output mapping above. */
                uint32_t destination = dsp_interleave ?
                    (i % channel_count) * block_count + i / channel_count : i;
                s->mem_write(s->mem_opaque, mem_space, mem_address + destination, v);
            }
        }

        if (s->error) { free(scratch_buf); return; }
        if (buffer_offset_writeback) {
            s->mem_write(s->mem_opaque, block_space, block_addr+4, scratch_offset);
        }
        free(scratch_buf);
    }
    /* Transfers above have completed; polling never manufactures completion. */
    if (s->next_block & NODE_POINTER_EOL) {
        s->eol = true;
        s->control &= ~DMA_CONTROL_RUNNING;
        s->control |= DMA_CONTROL_STOPPED;
    }
}

uint32_t dsp_dma_read(DSPDMAState *s, DSPDMARegister reg)
{
    switch (reg) {
    case DMA_CONFIGURATION:
        return s->configuration;
    case DMA_CONTROL:
        return s->control;
    case DMA_START_BLOCK:
        return s->start_block;
    case DMA_NEXT_BLOCK:
        return s->next_block;
    default:
        assert(!"Invalid register for dsp_dma_read");
    }
    return 0;
}

void dsp_dma_write(DSPDMAState *s, DSPDMARegister reg, uint32_t v)
{
    switch (reg) {
    case DMA_CONFIGURATION:
        s->configuration = v;
        break;
    case DMA_CONTROL:
        switch(v & DMA_CONTROL_ACTION) {
        case DMA_CONTROL_ACTION_NOP:
            break;
        case DMA_CONTROL_ACTION_START:
            s->control |= DMA_CONTROL_RUNNING;
            s->control &= ~DMA_CONTROL_STOPPED;
            break;
        case DMA_CONTROL_ACTION_STOP:
            s->control |= DMA_CONTROL_STOPPED;
            s->control &= ~DMA_CONTROL_RUNNING;
            break;
        case DMA_CONTROL_ACTION_FREEZE:
            s->control |= DMA_CONTROL_FROZEN;
            break;
        case DMA_CONTROL_ACTION_UNFREEZE:
            s->control &= ~DMA_CONTROL_FROZEN;
            break;
        default:
            /* ABORT and reserved actions lack verified hardware semantics. */
            fprintf(stderr, "Unsupported DSP DMA control action: %u\n", v & DMA_CONTROL_ACTION);
            s->error = true;
            s->control = (s->control & ~DMA_CONTROL_RUNNING) | DMA_CONTROL_STOPPED;
            break;
        }
        break;
    case DMA_START_BLOCK:
        s->start_block = v;
        break;
    case DMA_NEXT_BLOCK:
        s->next_block = v;
        break;
    default:
        assert(!"Invalid dma write register");
    }
}
