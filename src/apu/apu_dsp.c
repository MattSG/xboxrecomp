/*
 * QEMU MCPX Audio Processing Unit implementation
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
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

/* GP/EP glue adapted from xemu gp_ep.c; see dsp/PROVENANCE.md. */
#include "apu_state.h"
#include "fpconv.h"

static bool scatter_gather_rw(MCPXAPUState *d, uint32_t sge_base,
                              uint32_t max_sge, uint8_t *ptr, uint32_t addr,
                              size_t len, bool dir)
{
    (void)d;
    while (len) {
        uint32_t entry = addr / TARGET_PAGE_SIZE;
        uint32_t offset = addr % TARGET_PAGE_SIZE;
        if (entry > max_sge || entry > (UINT32_MAX - sge_base) / 8) return false;
        uint32_t page = ldl_le_phys(address_space_memory, sge_base + entry * 8);
        size_t chunk = TARGET_PAGE_SIZE - offset;
        if (chunk > len) chunk = len;
        uint8_t *physical = mcpx_apu_phys((uint64_t)page + offset);
        if (dir) memcpy(physical, ptr, chunk);
        else memcpy(ptr, physical, chunk);
        ptr += chunk;
        len -= chunk;
        if (len && addr > UINT32_MAX - chunk) return false;
        addr += (uint32_t)chunk;
    }
    return true;
}

static void gp_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    if (!scatter_gather_rw(d, d->regs[NV_PAPU_GPSADDR], d->regs[NV_PAPU_GPSMAXSGE],
                           ptr, addr, len, dir)) d->gp.dsp->dma.error = true;
}
static void ep_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    MCPXAPUState *d = opaque;
    if (!scatter_gather_rw(d, d->regs[NV_PAPU_EPSADDR], d->regs[NV_PAPU_EPSMAXSGE],
                           ptr, addr, len, dir)) d->ep.dsp->dma.error = true;
}

static void publish_pcm_frame(MCPXAPUState *d, const uint8_t *pcm)
{
    size_t size = sizeof(d->monitor.frame_buf);
    if ((d->monitor.ep_pcm_ready || d->monitor.frame_mixed) &&
        d->monitor.ep_pcm_capacity - d->monitor.ep_pcm_queued < size &&
        d->monitor.ep_pcm_queued < 64 * size) {
        size_t required = d->monitor.ep_pcm_queued + size;
        uint8_t *queue = realloc(d->monitor.ep_pcm_queue, required);
        if (queue) {
            d->monitor.ep_pcm_queue = queue;
            d->monitor.ep_pcm_capacity = required;
        }
    }
    if (!d->monitor.ep_pcm_ready && !d->monitor.frame_mixed) {
        memcpy(d->monitor.frame_buf, pcm, sizeof(d->monitor.frame_buf));
        d->monitor.ep_pcm_ready = true;
    } else if (d->monitor.ep_pcm_capacity - d->monitor.ep_pcm_queued >= sizeof(d->monitor.frame_buf)) {
        memcpy(d->monitor.ep_pcm_queue + d->monitor.ep_pcm_queued,
               pcm, sizeof(d->monitor.frame_buf));
        d->monitor.ep_pcm_queued += sizeof(d->monitor.frame_buf);
    } else {
        if (!d->monitor.ep_pcm_dropped_frames)
            fprintf(stderr, "[APU] Native PCM staging overflow; guest DSP output retained\n");
        d->monitor.ep_pcm_dropped_frames++;
    }
}

static void fifo_rw(MCPXAPUState *d, bool gp, uint8_t *ptr, unsigned index,
                     size_t len, bool dir)
{
    DSPState *dsp = gp ? d->gp.dsp : d->ep.dsp;
    if (index >= (dir ? 4u : 2u)) { dsp->dma.error = true; return; }
    uint32_t reg = (gp ? (dir ? NV_PAPU_GPOFBASE0 : NV_PAPU_GPIFBASE0)
                       : (dir ? NV_PAPU_EPOFBASE0 : NV_PAPU_EPIFBASE0)) + index * 0x10;
    uint32_t base = GET_MASK(d->regs[reg], NV_PAPU_GPOFBASE0_VALUE);
    uint32_t end = GET_MASK(d->regs[reg + 4], NV_PAPU_GPOFEND0_VALUE);
    uint32_t cur = GET_MASK(d->regs[reg + 8], NV_PAPU_GPOFCUR0_VALUE);
    if (base >= end) { dsp->dma.error = true; return; }
    if (cur < base) cur = base;
    if (cur >= end) cur = base + (cur - base) % (end - base);

    bool publish_pcm = !gp && dir && index == 0 &&
        (d->monitor.point == MCPX_APU_DEBUG_MON_EP ||
         d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP);
    /* ponytail: at most 64 pending native frames (~341ms). Host overflow
     * drops are counted separately; guest DMA must still execute normally. */
    if (publish_pcm) {
        size_t frame_size = sizeof(d->monitor.frame_buf);
        if (len > SIZE_MAX - d->monitor.ep_pcm_offset) { dsp->dma.error = true; return; }
        size_t frames = (d->monitor.ep_pcm_offset + len) / frame_size;
        if (!d->monitor.ep_pcm_ready && !d->monitor.frame_mixed && frames) frames--;
        if (frames > (SIZE_MAX - d->monitor.ep_pcm_queued) / frame_size) { dsp->dma.error = true; return; }
        size_t required = d->monitor.ep_pcm_queued + frames * frame_size;
        if (required > 64 * frame_size) required = 64 * frame_size;
        if (required > d->monitor.ep_pcm_capacity) {
            uint8_t *queue = realloc(d->monitor.ep_pcm_queue, required);
            if (queue) {
                d->monitor.ep_pcm_queue = queue;
                d->monitor.ep_pcm_capacity = required;
            }
        }
    }
    uint8_t *pcm = ptr;
    size_t pcm_len = len;
    uint32_t sge = d->regs[gp ? NV_PAPU_GPFADDR : NV_PAPU_EPFADDR];
    uint32_t max_sge = d->regs[gp ? NV_PAPU_GPFMAXSGE : NV_PAPU_EPFMAXSGE];
    while (len) {
        size_t chunk = end - cur;
        if (chunk > len) chunk = len;
        if (!scatter_gather_rw(d, sge, max_sge, ptr, cur, chunk, dir)) {
            dsp->dma.error = true;
            return;
        }
        ptr += chunk;
        len -= chunk;
        cur += (uint32_t)chunk;
        if (cur == end) cur = base;
    }
    /* Publish PCM only after the complete guest FIFO write succeeds. */
    if (publish_pcm) {
        size_t offset = 0;
        while (offset < pcm_len) {
            size_t chunk = sizeof(d->monitor.ep_pcm_buf) - d->monitor.ep_pcm_offset;
            if (chunk > pcm_len - offset) chunk = pcm_len - offset;
            memcpy(d->monitor.ep_pcm_buf + d->monitor.ep_pcm_offset, pcm + offset, chunk);
            offset += chunk;
            d->monitor.ep_pcm_offset += chunk;
            if (d->monitor.ep_pcm_offset == sizeof(d->monitor.ep_pcm_buf)) {
                publish_pcm_frame(d, d->monitor.ep_pcm_buf);
                d->monitor.ep_pcm_offset = 0;
            }
        }
        static unsigned traced_pcm;
        static bool traced_nonzero_pcm;
        if (getenv("RECOMP_APU_TRACE") && (traced_pcm < 8 || !traced_nonzero_pcm)) {
            unsigned nonzero = 0;
            for (size_t i = 0; i < pcm_len; i++) nonzero += pcm[i] != 0;
            if (traced_pcm++ < 8 || nonzero)
                fprintf(stderr, "[DSP] EP FIFO0 PCM bytes=%zu nonzero_bytes=%u\n", pcm_len, nonzero);
            traced_nonzero_pcm |= nonzero != 0;
        }
    }
    SET_MASK(d->regs[reg + 8], NV_PAPU_GPOFCUR0_VALUE, cur);
}
/* Called after native output accepts the current frame, with the APU lock held. */
void mcpx_apu_dsp_pcm_consumed(MCPXAPUState *d)
{
    d->monitor.ep_pcm_ready = false;
    d->monitor.frame_mixed = false;
    if (d->monitor.ep_pcm_queued >= sizeof(d->monitor.frame_buf)) {
        memcpy(d->monitor.frame_buf, d->monitor.ep_pcm_queue, sizeof(d->monitor.frame_buf));
        d->monitor.ep_pcm_queued -= sizeof(d->monitor.frame_buf);
        memmove(d->monitor.ep_pcm_queue,
                d->monitor.ep_pcm_queue + sizeof(d->monitor.frame_buf), d->monitor.ep_pcm_queued);
        d->monitor.ep_pcm_ready = true;
    }
}

static void gp_fifo_rw(void *opaque, uint8_t *ptr, unsigned index, size_t len, bool dir)
{
    fifo_rw(opaque, true, ptr, index, len, dir);
}
static void ep_fifo_rw(void *opaque, uint8_t *ptr, unsigned index, size_t len, bool dir)
{
    fifo_rw(opaque, false, ptr, index, len, dir);
}

/* Both processors use the same register map, with different memory sizes. */
static bool memory_window(bool gp, uint32_t addr, char *space, uint32_t *word)
{
    if (addr < (gp ? 0x1000u : 0xc00u) * 4) {
        *space = 'X'; *word = addr / 4;
    } else if (gp && addr >= NV_PAPU_GPMIXBUF && addr < NV_PAPU_GPMIXBUF + 0x400 * 4) {
        *space = 'X'; *word = GP_DSP_MIXBUF_BASE + (addr - NV_PAPU_GPMIXBUF) / 4;
    } else if (addr >= NV_PAPU_GPYMEM && addr < NV_PAPU_GPYMEM + (gp ? 0x800u : 0x100u) * 4) {
        *space = 'Y'; *word = (addr - NV_PAPU_GPYMEM) / 4;
    } else if (addr >= NV_PAPU_GPPMEM && addr < NV_PAPU_GPPMEM + 0x1000 * 4) {
        *space = 'P'; *word = (addr - NV_PAPU_GPPMEM) / 4;
    } else return false;
    return true;
}

uint64_t mcpx_apu_dsp_read(MCPXAPUState *d, bool gp, uint32_t addr, unsigned size)
{
    if ((size != 1 && size != 2 && size != 4 && size != 8) ||
        addr >= 0x10000 || size > 0x10000 - addr) return 0;
    DSPState *dsp = gp ? d->gp.dsp : d->ep.dsp;
    if (!dsp) return 0;
    char space;
    uint32_t word;
    qemu_mutex_lock(&d->lock);
    uint64_t result = 0;
    for (unsigned i = 0; i < size; ) {
        uint32_t aligned = (addr + i) & ~3u;
        uint32_t value = memory_window(gp, aligned, &space, &word)
            ? dsp_read_memory(dsp, space, word) : (gp ? d->gp.regs[aligned] : d->ep.regs[aligned]);
        unsigned lane = (addr + i) & 3u;
        while (lane < 4 && i < size) {
            result |= (uint64_t)((value >> (lane * 8)) & 0xff) << (i * 8);
            lane++; i++;
        }
    }
    qemu_mutex_unlock(&d->lock);
    return result;
}

static void dsp_write_word_locked(MCPXAPUState *d, bool gp, uint32_t addr, uint32_t value)
{
    DSPState *dsp = gp ? d->gp.dsp : d->ep.dsp;
    if (!dsp) return;
    uint32_t *regs = gp ? d->gp.regs : d->ep.regs;
    char space;
    uint32_t word;
    if (memory_window(gp, addr, &space, &word)) {
        dsp_write_memory(dsp, space, word, value & 0xffffff);
    } else {
        if (addr == NV_PAPU_GPRST) {
            const uint32_t enabled = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
            if ((value & enabled) != enabled) dsp_reset(dsp);
            else if ((regs[addr] & enabled) != enabled) {
                dsp_bootstrap(dsp);
                if (getenv("RECOMP_APU_TRACE"))
                    fprintf(stderr, "[DSP] %s bootstrap P[0..2]=%06X %06X %06X DMA.error=%d\n",
                            gp ? "GP" : "EP", dsp_read_memory(dsp, 'P', 0),
                            dsp_read_memory(dsp, 'P', 1), dsp_read_memory(dsp, 'P', 2), dsp->dma.error);
            }
            if (!gp && ((value & enabled) != enabled ||
                        (regs[addr] & enabled) != enabled)) {
                d->ep_frame_div = 0;
                d->monitor.ep_pcm_offset = 0;
                d->monitor.ep_pcm_queued = 0;
                d->monitor.ep_pcm_ready = false;
                d->monitor.frame_mixed = false;
            }
            d->pause_requested = false;
            qemu_cond_signal(&d->cond);
        }
        regs[addr] = value;
    }
}

void mcpx_apu_dsp_write(MCPXAPUState *d, bool gp, uint32_t addr, uint64_t value, unsigned size)
{
    if ((size != 1 && size != 2 && size != 4 && size != 8) ||
        addr >= 0x10000 || size > 0x10000 - addr) return;
    DSPState *dsp = gp ? d->gp.dsp : d->ep.dsp;
    if (!dsp) return;
    qemu_mutex_lock(&d->lock);
    for (unsigned i = 0; i < size; ) {
        uint32_t aligned = (addr + i) & ~3u;
        char space;
        uint32_t word;
        uint32_t merged = memory_window(gp, aligned, &space, &word)
            ? dsp_read_memory(dsp, space, word) : (gp ? d->gp.regs[aligned] : d->ep.regs[aligned]);
        unsigned lane = (addr + i) & 3u;
        while (lane < 4 && i < size) {
            merged = (merged & ~(0xffu << (lane * 8))) |
                     ((uint32_t)((value >> (i * 8)) & 0xff) << (lane * 8));
            lane++; i++;
        }
        dsp_write_word_locked(d, gp, aligned, merged);
    }
    qemu_mutex_unlock(&d->lock);
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    d->gp.realtime = d->ep.realtime = true;
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    d->gp.dsp = dsp_init(d, gp_scratch_rw, gp_fifo_rw, true);
    d->ep.dsp = dsp_init(d, ep_scratch_rw, ep_fifo_rw, false);
    if (!d->gp.dsp || !d->ep.dsp) {
        fprintf(stderr, "[APU] Cannot allocate DSP interpreter state\n");
        abort();
    }
    d->monitor.point = MCPX_APU_DEBUG_MON_GP_OR_EP;
    mcpx_apu_update_dsp_preference(d);
    fprintf(stderr, "[APU] GP/EP DSP56300 interpreters initialized\n");
}

/* Called with the APU lock held, as are the frame's native mixer operations. */
static void run_dsp_frame(MCPXAPUState *d, DSPState *dsp)
{
    static int profile = -1;
    static uint64_t ticks[2], frames[2], batches[2], peak[2];
    if (profile < 0) profile = getenv("RECOMP_DSP_PROFILE") != NULL;
    unsigned index = dsp->is_gp ? 0 : 1;
    uint64_t frame_ticks = 0;
    dsp_start_frame(dsp);
    do {
        LARGE_INTEGER begin, end;
        if (profile) QueryPerformanceCounter(&begin);
        dsp_run(dsp, 1000);
        if (profile) {
            QueryPerformanceCounter(&end);
            frame_ticks += (uint64_t)(end.QuadPart - begin.QuadPart);
            batches[index]++;
        }
        if (!dsp_has_work(dsp) || qatomic_read(&d->exiting)) break;
        /* Guest MMIO must be able to reset or command a continuously running DSP. */
        qemu_mutex_unlock(&d->lock);
        Sleep(0);
        qemu_mutex_lock(&d->lock);
        uint32_t reset = dsp->is_gp ? d->gp.regs[NV_PAPU_GPRST] : d->ep.regs[NV_PAPU_EPRST];
        uint32_t enabled = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
        if ((reset & enabled) != enabled) break;
    } while (dsp_has_work(dsp) && !qatomic_read(&d->exiting));
    if (profile) {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        ticks[index] += frame_ticks;
        if (frame_ticks > peak[index]) peak[index] = frame_ticks;
        if (++frames[index] % 1024 == 0)
            fprintf(stderr, "[DSP_PROFILE] %s frames=%llu batches=%llu total_ms=%.3f mean_us=%.3f peak_us=%.3f\n",
                    dsp->is_gp ? "GP" : "EP",
                    (unsigned long long)frames[index], (unsigned long long)batches[index],
                    1000.0*ticks[index]/frequency.QuadPart,
                    1000000.0*ticks[index]/frequency.QuadPart/frames[index],
                    1000000.0*peak[index]/frequency.QuadPart);
    }
}

void mcpx_apu_dsp_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    for (int bin = 0; bin < NUM_MIXBINS; bin++)
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            dsp_write_memory(d->gp.dsp, 'X', GP_DSP_MIXBUF_BASE + bin * NUM_SAMPLES_PER_FRAME + i,
                             float_to_24b(mixbins[bin][i]));
    const uint32_t enabled = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    bool ep_enabled = (d->ep.regs[NV_PAPU_EPRST] & enabled) == enabled;
    if ((d->gp.regs[NV_PAPU_GPRST] & enabled) == enabled) {
        run_dsp_frame(d, d->gp.dsp);
        if (qatomic_read(&d->exiting)) return;
        ep_enabled = (d->ep.regs[NV_PAPU_EPRST] & enabled) == enabled;
        g_dbg.gp.cycles = d->gp.dsp->core.cycle_count;
        static unsigned gp_trace_frames;
        if (getenv("RECOMP_APU_TRACE") && gp_trace_frames++ < 8)
            fprintf(stderr, "[DSP] GP frame cycles=%llu idle=%d DMA.error=%d EOL=%d\n",
                    (unsigned long long)g_dbg.gp.cycles, d->gp.dsp->core.is_idle,
                    d->gp.dsp->dma.error, d->gp.dsp->dma.eol);
        if (d->monitor.point == MCPX_APU_DEBUG_MON_GP ||
            (d->monitor.point == MCPX_APU_DEBUG_MON_GP_OR_EP && !ep_enabled)) {
            int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;
            for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
                ((int16_t (*)[2])d->monitor.ep_pcm_buf)[off + i][0] = dsp_read_memory(d->gp.dsp, 'X', 0x1400 + i) >> 8;
                ((int16_t (*)[2])d->monitor.ep_pcm_buf)[off + i][1] = dsp_read_memory(d->gp.dsp, 'X', 0x1420 + i) >> 8;
            }
            if (d->ep_frame_div % 8 == 7) publish_pcm_frame(d, d->monitor.ep_pcm_buf);
        }
    }
    if (ep_enabled && d->ep_frame_div % 8 == 0) {
        run_dsp_frame(d, d->ep.dsp);
        g_dbg.ep.cycles = d->ep.dsp->core.cycle_count;
        static unsigned ep_trace_frames;
        if (getenv("RECOMP_APU_TRACE") && ep_trace_frames++ < 8)
            fprintf(stderr, "[DSP] EP frame cycles=%llu idle=%d DMA.error=%d EOL=%d\n",
                    (unsigned long long)g_dbg.ep.cycles, d->ep.dsp->core.is_idle,
                    d->ep.dsp->dma.error, d->ep.dsp->dma.eol);
    }
}
