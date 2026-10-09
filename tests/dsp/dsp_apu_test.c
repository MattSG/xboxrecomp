#include "apu_state.h"
#include "dsp/dsp_dma_regs.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "APU DSP check failed at line %d: %s\n", __LINE__, #condition); \
    exit(1); } } while (0)

struct McpxApuDebug g_dbg;
int dsp_test_fail_alloc;
static uint8_t ram[0x20000];
static void check_capture(const char *prefix, const char *processor)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s.%s.frame.bin", prefix, processor);
    FILE *file = fopen(path, "rb");
    CHECK(file);
    uint32_t header[3], event[5];
    DSPState state;
    CHECK(fread(header, sizeof(header), 1, file) == 1);
    CHECK(header[0] == 0x44535046 && header[1] == DSP_FRAME_CAPTURE_VERSION && header[2] == sizeof(state));
    CHECK(fread(&state, sizeof(state), 1, file) == 1 && !state.core.is_idle);
    unsigned transfers = 0;
    for (;;) {
        CHECK(fread(event, sizeof(event), 1, file) == 1);
        if (!event[0]) break;
        CHECK(event[0] <= 2 && event[3] <= 1 && event[4] <= 1);
        CHECK(event[2] <= 0x100000);
        if (!event[4]) {
            uint8_t *payload = malloc(event[2] ? event[2] : 1);
            CHECK(payload && fread(payload, 1, event[2], file) == event[2]);
            if (!strcmp(processor, "ep")) {
                CHECK(event[0] == 2 && event[1] == 0 && event[2] == 1024 && event[3] == 1);
                CHECK(payload[0] == 0x34 && payload[1] == 0x12);
            }
            free(payload);
        }
        transfers++;
    }
    CHECK(fread(&state, sizeof(state), 1, file) == 1 && state.core.is_idle);
    CHECK(!state.core.opaque && !state.core.read_peripheral && !state.dma.rw_opaque);
    CHECK(fgetc(file) == EOF);
    CHECK(fclose(file) == 0);
    if (!strcmp(processor, "ep")) CHECK(transfers == 1);
}
uint8_t *g_apu_ram_ptr = ram;
uint8_t *mcpx_apu_phys(uint64_t addr)
{
    CHECK(addr < sizeof(ram));
    return ram + addr;
}

static void run_locked_frame(MCPXAPUState *d, float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    qemu_mutex_lock(&d->lock);
    mcpx_apu_dsp_frame(d, mixbins);
    qemu_mutex_unlock(&d->lock);
}

static DWORD WINAPI looping_frame(void *opaque)
{
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {0};
    run_locked_frame(opaque, mixbins);
    return 0;
}

int main(void)
{
    MCPXAPUState *d = calloc(1, sizeof(*d));
    CHECK(d);
    d->ram_ptr = ram;
    qemu_mutex_init(&d->lock);
    qemu_cond_init(&d->cond);
    mcpx_apu_dsp_init(d);
    if (getenv("DSP_TEST_CAPTURE_WAIT")) {
        DSPState *gp = d->gp.dsp;
        d->regs[NV_PAPU_GPSADDR] = 0x1000;
        d->regs[NV_PAPU_GPSMAXSGE] = 0;
        stl_le_phys(address_space_memory, 0x1000, 0x4000);
        const uint32_t program[] = {0x44f400, 0x20, 0x08c414,
                                   0x44f400, 1, 0x08c416, 0x86};
        for (unsigned i = 0; i < sizeof(program)/sizeof(program[0]); i++)
            dsp_write_memory(gp, 'P', i, program[i]);
        dsp_write_memory(gp, 'X', 0xf00, 0x654321);
        for (unsigned node = 0; node < 512; node++) {
            const uint32_t descriptor[] = {node == 511 ? 1u << 14 : 0x20 + (node + 1) * 7,
                (0xf << 5) | (6 << 10) | 2, 1, 0xf00, node * 4, 0, 0};
            for (unsigned i = 0; i < 7; i++)
                dsp_write_memory(gp, 'X', 0x20 + node * 7 + i, descriptor[i]);
            stl_le_phys(address_space_memory, 0x4000 + node * 4, 0xffffffff);
        }
        dsp_start_frame(gp);
        dsp_run(gp, 1000);
        CHECK(gp->core.is_waiting && dsp_has_work(gp) && !gp->dma.eol);
        CHECK(ldl_le_phys(address_space_memory, 0x47fc) == 0xffffffff);
        dsp_run(gp, 1000);
        CHECK(gp->core.is_waiting && !dsp_has_work(gp) && gp->dma.eol && !gp->dma.error);
        for (unsigned node = 0; node < 512; node++)
            CHECK(ldl_le_phys(address_space_memory, 0x4000 + node * 4) == 0x654321);
        check_capture(getenv("RECOMP_DSP_FRAME_DUMP"), "gp");
        free(d->gp.dsp); free(d->ep.dsp); free(d);
        puts("WAIT capture retained 512 real transfers across scheduler batches");
        return 0;
    }
    if (!getenv("RECOMP_DSP_FRAME_DUMP")) {
        DSPState *gp = d->gp.dsp;
        CHECK(sizeof(DSPState) == 80128); /* Preserve existing frame capture ABI. */
        d->regs[NV_PAPU_GPSADDR] = 0x1000;
        d->regs[NV_PAPU_GPSMAXSGE] = 0;
        stl_le_phys(address_space_memory, 0x1000, 0x4000);
        const uint32_t reset_descriptor[] = {1u << 14, (0xf << 5) | (6 << 10) | 2,
                                            1, 0x200, 0x100, 0, 0};
        for (unsigned faulted = 0; faulted < 2; faulted++) {
            dsp_reset(gp);
            dsp_write_memory(gp, 'P', 0, 0x84); /* Software RESET, not CPU reset. */
            dsp_write_memory(gp, 'P', 1, 0x014180); /* ADD #1,A follows RESET. */
            dsp_write_memory(gp, 'X', 0x200, 0x123456);
            dsp_write_memory(gp, 'Y', 0x200, 0x987654);
            for (unsigned i = 0; i < 7; i++)
                dsp_write_memory(gp, 'X', 0x20 + i, reset_descriptor[i]);
            stl_le_phys(address_space_memory, 0x4100, 0xffffffff);
            gp->core.registers[DSP_REG_X0] = 0x654321;
            gp->core.registers[DSP_REG_SR] = 0x300;
            gp->core.stack[0][1] = 0x123;
            gp->core.periph[0] = 0xabcdef;
            gp->interrupts = 0xff;
            gp->dma.configuration = 0xabcdef;
            gp->dma.control = DMA_CONTROL_RUNNING;
            gp->dma.start_block = gp->dma.next_block = 0x20;
            gp->dma.error = gp->dma.eol = faulted != 0;
            void *saved_opaque = gp->dma.rw_opaque;
            dsp_scratch_rw_func saved_scratch = gp->dma.scratch_rw;
            dsp_fifo_rw_func saved_fifo = gp->dma.fifo_rw;
            dsp_run(gp, 2);
            CHECK(gp->core.pc == 1 && gp->core.registers[DSP_REG_X0] == 0x654321);
            CHECK(gp->core.registers[DSP_REG_SR] == 0x300 && gp->core.stack[0][1] == 0x123);
            CHECK(!gp->core.periph[0] && !gp->interrupts);
            CHECK(!gp->dma.configuration && !gp->dma.control && !gp->dma.start_block && !gp->dma.next_block);
            CHECK(!gp->dma.error && !gp->dma.eol && gp->dma.rw_opaque == saved_opaque);
            CHECK(gp->dma.scratch_rw == saved_scratch && gp->dma.fifo_rw == saved_fifo);
            CHECK(ldl_le_phys(address_space_memory, 0x4100) == 0xffffffff);
            CHECK(dsp_read_memory(gp, 'X', 0x200) == 0x123456);
            CHECK(dsp_read_memory(gp, 'Y', 0x200) == 0x987654);
            CHECK(dsp_read_memory(gp, 'X', 0x20) == reset_descriptor[0]);
            dsp_run(gp, 4);
            CHECK(gp->core.pc == 2 && gp->core.registers[DSP_REG_A1] == 1);
            CHECK(!gp->dma.eol && ldl_le_phys(address_space_memory, 0x4100) == 0xffffffff);
        }
        /* Positive control: the same configured descriptor really writes RAM. */
        gp->dma.next_block = 0x20;
        dsp_dma_write(&gp->dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
        dsp_dma_step(&gp->dma);
        CHECK(!gp->dma.error && gp->dma.eol);
        CHECK(ldl_le_phys(address_space_memory, 0x4100) == 0x123456);
        dsp_reset(gp);
        dsp_write_memory(gp, 'P', 0, 0x86); /* WAIT */
        dsp_write_memory(gp, 'P', 1, 0x014180); /* ADD #1,A must not run asleep. */
        dsp_run(gp, 1000);
        CHECK(gp->core.is_waiting && gp->core.is_idle && gp->core.pc == 1);
        uint32_t cycles = gp->core.cycle_count;
        dsp_start_frame(gp);
        dsp_run(gp, 1000);
        CHECK(gp->core.is_waiting && gp->core.pc == 1 && gp->core.registers[DSP_REG_A1] == 0);
        CHECK(gp->core.cycle_count == 0 && cycles == 2 && gp->save_cycles == 0);
        dsp56k_add_interrupt(&gp->core, DSP_INTER_TRAP);
        CHECK(!gp->core.is_waiting && !gp->core.is_idle);
        dsp_reset(gp);
        CHECK(!gp->core.is_waiting && !gp->core.is_idle && gp->core.pc == 0);
    }
    if (getenv("DSP_TEST_CAPTURE_RESET")) {
        DSPState *gp = d->gp.dsp;
        void *opaque = gp->dma.rw_opaque;
        dsp_scratch_rw_func scratch = gp->dma.scratch_rw;
        dsp_fifo_rw_func fifo = gp->dma.fifo_rw;
        dsp_write_memory(gp, 'P', 0, 0x44f400);
        dsp_write_memory(gp, 'P', 1, 1);
        dsp_write_memory(gp, 'P', 2, 0x08c404);
        dsp_start_frame(gp);
        CHECK(gp->dma.rw_opaque != opaque);
        dsp_reset(gp);
        CHECK(gp->dma.rw_opaque == opaque);
        CHECK(gp->dma.scratch_rw == scratch && gp->dma.fifo_rw == fifo);
        char path[1024];
        snprintf(path, sizeof(path), "%s.gp.frame.bin", getenv("RECOMP_DSP_FRAME_DUMP"));
        FILE *file = fopen(path, "rb");
        CHECK(file && fseek(file, 0, SEEK_END) == 0);
        CHECK(ftell(file) == 12 + sizeof(DSPState));
        CHECK(fclose(file) == 0);
        /* After reset, FIFO reaches the original guest RAM callback again. */
        uint8_t payload[4] = {0};
        gp->dma.fifo_rw(gp->dma.rw_opaque, payload, 0, sizeof(payload), true);
        CHECK(gp->dma.error); /* unconfigured ring, handled without capture */
        free(d->gp.dsp); free(d->ep.dsp); free(d);
        puts("Interrupted capture closed and real DMA callbacks restored");
        return 0;
    }
    d->regs[NV_PAPU_GPSADDR] = 0x1000;
    d->regs[NV_PAPU_GPSMAXSGE] = 1;
    stl_le_phys(address_space_memory, 0x1000, 0x4000);
    stl_le_phys(address_space_memory, 0x1008, 0x7000);

    if (!getenv("RECOMP_DSP_FRAME_DUMP")) {
        DSPState *gp = d->gp.dsp;
        const uint32_t wait_program[] = {0x44f400, 0x20, 0x08c414,
                                        0x44f400, 1, 0x08c416, 0x86, 0x014180};
        for (unsigned i = 0; i < sizeof(wait_program)/sizeof(wait_program[0]); i++)
            dsp_write_memory(gp, 'P', i, wait_program[i]);
        for (unsigned node = 0; node < 3; node++) {
            const uint32_t descriptor[] = {node == 2 ? 1u << 14 : 0x30 + node * 0x10,
                (0xf << 5) | (6 << 10) | 2, 1, 0x200 + node, 0x100 + node * 4, 0, 0};
            for (unsigned i = 0; i < 7; i++)
                dsp_write_memory(gp, 'X', 0x20 + node * 0x10 + i, descriptor[i]);
            dsp_write_memory(gp, 'X', 0x200 + node, 0x123450 + node);
            stl_le_phys(address_space_memory, 0x4100 + node * 4, 0xffffffff);
        }
        dsp_dma_write(&gp->dma, DMA_CONTROL, DMA_CONTROL_ACTION_FREEZE);
        dsp_run(gp, 1000);
        CHECK(gp->core.is_waiting && gp->core.pc == 7 && !gp->dma.eol);
        CHECK(ldl_le_phys(address_space_memory, 0x4100) == 0xffffffff);
        dsp_dma_write(&gp->dma, DMA_CONTROL, DMA_CONTROL_ACTION_UNFREEZE);
        dsp_run(gp, 2);
        CHECK(ldl_le_phys(address_space_memory, 0x4100) == 0x123450);
        CHECK(!gp->dma.eol && gp->dma.next_block == 0x30);
        dsp_run(gp, 1000);
        CHECK(gp->dma.eol && !gp->dma.error && gp->core.is_waiting && gp->core.pc == 7);
        CHECK(gp->core.registers[DSP_REG_A1] == 0);
        CHECK(ldl_le_phys(address_space_memory, 0x4104) == 0x123451);
        CHECK(ldl_le_phys(address_space_memory, 0x4108) == 0x123452);
        dsp_reset(gp);
        dsp_write_memory(gp, 'P', 6, 0x87); /* Same program, STOP rather than WAIT. */
        for (unsigned node = 0; node < 3; node++)
            stl_le_phys(address_space_memory, 0x4100 + node * 4, 0xffffffff);
        dsp_run(gp, 1000);
        CHECK(gp->core.is_stopped && gp->core.pc == 7 && !gp->dma.eol);
        CHECK(!dsp_has_work(gp) && (gp->dma.control & DMA_CONTROL_RUNNING));
        dsp_start_frame(gp);
        dsp_run(gp, 1000);
        CHECK(gp->core.is_stopped && gp->core.pc == 7 && gp->core.cycle_count == 0);
        CHECK(gp->core.registers[DSP_REG_A1] == 0);
        for (unsigned node = 0; node < 3; node++)
            CHECK(ldl_le_phys(address_space_memory, 0x4100 + node * 4) == 0xffffffff);
        dsp_reset(gp);
        CHECK(!gp->core.is_stopped && !gp->core.is_waiting && !gp->core.is_idle);
    }

    /* Uploaded guest program: start a DMA descriptor, then halt this frame.
     * move #imm,X0; movep X0,x:peripheral. No host completion call. */
    const uint32_t program[] = {0x44f400, 0x20, 0x08c414,
                               0x44f400, 1, 0x08c416,
                               0x44f400, 1, 0x08c404,
                               0x0c0000};
    memcpy(ram + 0x4000, program, sizeof(program));
    /* Bootstrap must read both scatter/gather pages and discard high bits. */
    stl_le_phys(address_space_memory, 0x7000, 0xab123456);
    mcpx_apu_dsp_write(d, true, NV_PAPU_GPRST, 3, 4);
    CHECK(d->gp.dsp->core.pram[0x400] == 0x123456);
    CHECK(mcpx_apu_dsp_read(d, true, NV_PAPU_GPPMEM, 4) == program[0]);
    CHECK(mcpx_apu_dsp_read(d, true, NV_PAPU_GPPMEM + 1, 1) == 0xf4);
    CHECK(mcpx_apu_dsp_read(d, true, NV_PAPU_GPPMEM + 1, 2) == 0x44f4);
    CHECK(mcpx_apu_dsp_read(d, true, NV_PAPU_GPPMEM, 8) == ((uint64_t)0x20 << 32 | 0x44f400));
    CHECK(mcpx_apu_dsp_read(d, true, NV_PAPU_GPPMEM + 3, 2) == 0x2000);
    mcpx_apu_dsp_write(d, true, 0x300, 0x65432100123456ull, 8);
    CHECK(mcpx_apu_dsp_read(d, true, 0x300, 8) == 0x65432100123456ull);
    mcpx_apu_dsp_write(d, true, 0x301, 0xab, 1);
    CHECK(mcpx_apu_dsp_read(d, true, 0x300, 4) == 0x12ab56);
    mcpx_apu_dsp_write(d, true, 0x303, 0xcdef, 2);
    CHECK(mcpx_apu_dsp_read(d, true, 0x300, 8) == 0x6543cd0012ab56ull);

    /* DSP clears a command word via an actual descriptor crossing an SGE page. */
    const uint32_t descriptor[] = {1u << 14, (0xf << 5) | (6 << 10) | 2,
                                  2, 0x100, 0xffc, 0, 0};
    for (unsigned i = 0; i < 7; i++)
        mcpx_apu_dsp_write(d, true, NV_PAPU_GPXMEM + (0x20 + i) * 4, descriptor[i], 4);
    mcpx_apu_dsp_write(d, true, NV_PAPU_GPXMEM + 0x100 * 4, 0, 4);
    mcpx_apu_dsp_write(d, true, NV_PAPU_GPXMEM + 0x101 * 4, 0x654321, 4);
    stl_le_phys(address_space_memory, 0x4ffc, 0xffffffff);
    /* START is observable before any transfer. Polls cannot move the engine;
     * the next guest instruction boundary performs the descriptor. */
    for (unsigned i = 0; i < 32 && d->gp.dsp->core.pc != 6; i++)
        dsp_run(d->gp.dsp, 1);
    CHECK(d->gp.dsp->core.pc == 6);
    for (unsigned i = 0; i < 10; i++)
        CHECK(dsp_read_memory(d->gp.dsp, 'X', 0xffffd6) & DMA_CONTROL_RUNNING);
    CHECK(ldl_le_phys(address_space_memory, 0x4ffc) == 0xffffffff);
    CHECK(!d->gp.dsp->dma.eol);
    for (unsigned i = 0; i < 32 && !d->gp.dsp->dma.eol; i++)
        dsp_run(d->gp.dsp, 1);
    CHECK(d->gp.dsp->dma.eol);
    CHECK(!(dsp_read_memory(d->gp.dsp, 'X', 0xffffd6) & DMA_CONTROL_RUNNING));
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME] = {0};
    mixbins[0][0] = 0.5f;
    run_locked_frame(d, mixbins);
    CHECK(ldl_le_phys(address_space_memory, 0x4ffc) == 0);
    CHECK(ldl_le_phys(address_space_memory, 0x7000) == 0x654321);
    CHECK(d->gp.dsp->core.is_idle && d->gp.dsp->dma.eol && g_dbg.gp.cycles > 0);
    CHECK(((int16_t (*)[2])d->monitor.ep_pcm_buf)[0][0] == 16384); /* First GP slice is staged. */
    CHECK(d->ep.dsp->core.num_inst == 0);
    CHECK(dsp_read_memory(d->gp.dsp, 'X', 0xffffc5) & (1u << 7));
    dsp_write_memory(d->gp.dsp, 'X', 0xffffc5, (1u << 7) | 2);
    CHECK(!d->gp.dsp->dma.eol && d->gp.dsp->interrupts == 0);

    mcpx_apu_dsp_write(d, true, NV_PAPU_GPRST, 0, 4);
    CHECK(d->gp.dsp->core.pc == 0 && d->gp.dsp->dma.control == 0);

    /* An invalid SGE cannot be acknowledged or reported as a completed DMA. */
    mcpx_apu_dsp_write(d, true, NV_PAPU_GPRST, 3, 4);
    for (unsigned i = 0; i < 7; i++)
        mcpx_apu_dsp_write(d, true, (0x20 + i) * 4, descriptor[i], 4);
    mcpx_apu_dsp_write(d, true, 0x24 * 4, 0x2000, 4);
    stl_le_phys(address_space_memory, 0x4ffc, 0xffffffff);
    run_locked_frame(d, mixbins);
    CHECK(d->gp.dsp->dma.error && !d->gp.dsp->dma.eol);
    CHECK(ldl_le_phys(address_space_memory, 0x4ffc) == 0xffffffff);
    mcpx_apu_dsp_write(d, true, NV_PAPU_GPRST, 0, 4);

    /* EP program outputs a complete PCM frame through a wrapping FIFO. */
    d->regs[NV_PAPU_EPSADDR] = 0x1100;
    d->regs[NV_PAPU_EPSMAXSGE] = 1;
    stl_le_phys(address_space_memory, 0x1100, 0x8000);
    stl_le_phys(address_space_memory, 0x1108, 0xa000);
    memcpy(ram + 0x8000, program, sizeof(program));
    d->regs[NV_PAPU_EPFADDR] = 0x1200;
    d->regs[NV_PAPU_EPFMAXSGE] = 1;
    stl_le_phys(address_space_memory, 0x1200, 0xb000);
    stl_le_phys(address_space_memory, 0x1208, 0xc000);
    d->regs[NV_PAPU_EPOFBASE0] = 0x1000;
    d->regs[NV_PAPU_EPOFEND0] = 0x1800;
    d->regs[NV_PAPU_EPOFCUR0] = 0x17fc;
    mcpx_apu_dsp_write(d, false, NV_PAPU_EPRST, 3, 4);
    const uint32_t ep_node[] = {1u << 14, (1 << 10) | 2, 512, 0x100, 0, 0, 0};
    for (unsigned i = 0; i < 7; i++)
        mcpx_apu_dsp_write(d, false, (0x20 + i) * 4, ep_node[i], 4);
    for (unsigned i = 0; i < 512; i++)
        mcpx_apu_dsp_write(d, false, (0x100 + i) * 4,
                           (i & 1) ? 0xfecc00 : 0x123400, 4);
    run_locked_frame(d, mixbins);
    CHECK(d->monitor.frame_buf[0][0] == 0x1234);
    CHECK(d->monitor.frame_buf[255][1] == (int16_t)0xfecc);
    CHECK(ldl_le_phys(address_space_memory, 0xc7fc) == 0xfecc1234);
    CHECK(ldl_le_phys(address_space_memory, 0xc000) == 0xfecc1234);
    CHECK(d->regs[NV_PAPU_EPOFCUR0] == 0x13fc);
    CHECK(d->ep.dsp->core.is_idle && d->ep.dsp->dma.eol && g_dbg.ep.cycles > 0);
    /* The same PCM frame split across two real FIFO descriptors. */
    mcpx_apu_dsp_write(d, false, NV_PAPU_EPRST, 0, 4);
    mcpx_apu_dsp_write(d, false, NV_PAPU_EPRST, 3, 4);
    const uint32_t split_node[] = {0x30, (1 << 10) | 2, 256, 0x100, 0, 0, 0};
    for (unsigned i = 0; i < 7; i++) {
        mcpx_apu_dsp_write(d, false, (0x20 + i) * 4, split_node[i], 4);
        mcpx_apu_dsp_write(d, false, (0x30 + i) * 4, ep_node[i], 4);
    }
    mcpx_apu_dsp_write(d, false, 0x32 * 4, 256, 4);
    mcpx_apu_dsp_write(d, false, 0x33 * 4, 0x200, 4);
    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
    run_locked_frame(d, mixbins);
    CHECK(!d->ep.dsp->dma.error && d->ep.dsp->dma.eol);
    CHECK(d->monitor.ep_pcm_offset == 0);
    CHECK(d->monitor.frame_buf[0][0] == 0x1234);
    CHECK(d->monitor.frame_buf[255][1] == (int16_t)0xfecc);
    CHECK(d->regs[NV_PAPU_EPOFCUR0] == 0x17fc);
    /* Partial EP PCM survives the native monitor clearing its output buffer. */
    mcpx_apu_dsp_pcm_consumed(d);
    uint8_t halves[1024];
    memset(halves, 0x12, 512);
    memset(halves + 512, 0x34, 512);
    memset(d->monitor.frame_buf, 0x5a, sizeof(d->monitor.frame_buf));
    d->ep.dsp->dma.fifo_rw(d->ep.dsp->dma.rw_opaque, halves, 0, 512, true);
    CHECK(!d->ep.dsp->dma.error && d->monitor.ep_pcm_offset == 512);
    for (unsigned i = 0; i < sizeof(d->monitor.frame_buf); i++)
        CHECK(((uint8_t *)d->monitor.frame_buf)[i] == 0x5a);
    memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
    d->ep.dsp->dma.fifo_rw(d->ep.dsp->dma.rw_opaque, halves + 512, 0, 512, true);
    CHECK(!d->ep.dsp->dma.error && d->monitor.ep_pcm_offset == 0);
    CHECK(!memcmp(d->monitor.frame_buf, halves, sizeof(halves)));
    mcpx_apu_dsp_pcm_consumed(d);
    /* One real FIFO transfer contains two frames plus half of a third. */
    uint8_t multi[2560];
    memset(multi, 0x11, 1024); memset(multi + 1024, 0x22, 1024); memset(multi + 2048, 0x33, 512);
    mcpx_apu_dsp_write(d, false, NV_PAPU_EPRST, 0, 4);
    mcpx_apu_dsp_write(d, false, NV_PAPU_EPRST, 3, 4);
    for (unsigned i = 0; i < 7; i++)
        mcpx_apu_dsp_write(d, false, (0x20 + i) * 4, ep_node[i], 4);
    mcpx_apu_dsp_write(d, false, 0x22 * 4, 1280, 4);
    for (unsigned i = 0; i < 1280; i++) {
        uint32_t value = i < 512 ? 0x111100 : i < 1024 ? 0x222200 : 0x333300;
        mcpx_apu_dsp_write(d, false, (0x100 + i) * 4, value, 4);
    }
    unsigned before_multi = d->ep.dsp->core.num_inst;
    run_locked_frame(d, mixbins);
    CHECK(d->ep.dsp->core.num_inst > before_multi && d->ep.dsp->core.is_idle && d->ep.dsp->dma.eol);
    CHECK(!d->ep.dsp->dma.error && d->monitor.ep_pcm_queued == 1024 && d->monitor.ep_pcm_offset == 512);
    CHECK(!memcmp(d->monitor.frame_buf, multi, 1024));
    mcpx_apu_dsp_pcm_consumed(d);
    CHECK(d->monitor.ep_pcm_queued == 0 && !memcmp(d->monitor.frame_buf, multi + 1024, 1024));
    mcpx_apu_dsp_pcm_consumed(d);
    mcpx_apu_dsp_write(d, false, 0x22 * 4, 256, 4);
    mcpx_apu_dsp_write(d, false, 0x23 * 4, 0x600, 4);
    for (unsigned i = 0; i < 256; i++)
        mcpx_apu_dsp_write(d, false, (0x600 + i) * 4, 0x333300, 4);
    before_multi = d->ep.dsp->core.num_inst;
    run_locked_frame(d, mixbins);
    CHECK(d->ep.dsp->core.num_inst > before_multi && d->ep.dsp->core.is_idle);
    CHECK(!d->ep.dsp->dma.error && d->monitor.ep_pcm_offset == 0 && d->monitor.ep_pcm_ready);
    for (unsigned i = 0; i < 1024; i++) CHECK(((uint8_t *)d->monitor.frame_buf)[i] == 0x33);
    /* New DSP PCM queues behind a mixed native-only frame retained on failure. */
    mcpx_apu_dsp_pcm_consumed(d);
    memset(d->monitor.frame_buf, 0x55, sizeof(d->monitor.frame_buf));
    d->monitor.frame_mixed = true;
    run_locked_frame(d, mixbins);
    run_locked_frame(d, mixbins); /* Two 512-byte guest FIFO writes form one host frame. */
    CHECK(!d->monitor.ep_pcm_ready && d->monitor.frame_mixed && d->monitor.ep_pcm_queued == 1024);
    for (unsigned i = 0; i < 1024; i++) CHECK(((uint8_t *)d->monitor.frame_buf)[i] == 0x55);
    mcpx_apu_dsp_pcm_consumed(d);
    CHECK(!d->monitor.frame_mixed && d->monitor.ep_pcm_ready && !d->monitor.ep_pcm_queued);
    for (unsigned i = 0; i < 1024; i++) CHECK(((uint8_t *)d->monitor.frame_buf)[i] == 0x33);
    /* A stalled native consumer cannot turn valid guest DMA into failure. */
    mcpx_apu_dsp_write(d, false, 0x22 * 4, 512, 4);
    mcpx_apu_dsp_write(d, false, 0x23 * 4, 0x100, 4);
    for (unsigned i = 0; i < 70; i++) {
        before_multi = d->ep.dsp->core.num_inst;
        run_locked_frame(d, mixbins);
        CHECK(d->ep.dsp->core.num_inst > before_multi && !d->ep.dsp->dma.error && d->ep.dsp->dma.eol);
    }
    CHECK(d->monitor.ep_pcm_capacity == 64 * 1024 && d->monitor.ep_pcm_queued == 64 * 1024);
    CHECK(d->monitor.ep_pcm_dropped_frames == 6);
    for (unsigned i = 0; i < 1024; i++) CHECK(((uint8_t *)d->monitor.frame_buf)[i] == 0x33);
    /* Native OOM must not suppress the guest FIFO write or fake an error. */
    free(d->monitor.ep_pcm_queue);
    d->monitor.ep_pcm_queue = NULL;
    d->monitor.ep_pcm_queued = d->monitor.ep_pcm_capacity = 0;
    uint32_t cur_before_oom = d->regs[NV_PAPU_EPOFCUR0];
    uint64_t dropped_before_oom = d->monitor.ep_pcm_dropped_frames;
    dsp_test_fail_alloc = 1;
    before_multi = d->ep.dsp->core.num_inst;
    run_locked_frame(d, mixbins);
    dsp_test_fail_alloc = 0;
    CHECK(d->ep.dsp->core.num_inst > before_multi && !d->ep.dsp->dma.error && d->ep.dsp->dma.eol);
    CHECK(d->regs[NV_PAPU_EPOFCUR0] == 0x1000 + (cur_before_oom - 0x1000 + 1024) % 0x800);
    CHECK(d->monitor.ep_pcm_dropped_frames == dropped_before_oom + 1);
    CHECK(!d->monitor.ep_pcm_queue && !d->monitor.ep_pcm_capacity && !d->monitor.ep_pcm_queued);
    for (unsigned i = 0; i < 1024; i++) {
        CHECK(ram[0xc000 + (cur_before_oom - 0x1000 + i) % 0x800] == 0x11);
        CHECK(((uint8_t *)d->monitor.frame_buf)[i] == 0x33);
    }
    run_locked_frame(d, mixbins);
    CHECK(!d->ep.dsp->dma.error && d->monitor.ep_pcm_queued == 1024);
    CHECK(d->monitor.ep_pcm_dropped_frames == dropped_before_oom + 1);
    mcpx_apu_dsp_pcm_consumed(d);
    for (unsigned i = 0; i < 1024; i++) CHECK(((uint8_t *)d->monitor.frame_buf)[i] == 0x11);
    /* Rewriting an already released reset register must not discard PCM. */
    d->monitor.ep_pcm_offset = 512;
    mcpx_apu_dsp_write(d, false, NV_PAPU_EPRST, 3, 4);
    CHECK(d->monitor.ep_pcm_offset == 512);
    mcpx_apu_dsp_write(d, false, NV_PAPU_EPRST, 0, 4);
    CHECK(d->monitor.ep_pcm_offset == 0 && d->monitor.ep_pcm_queued == 0 && !d->monitor.ep_pcm_ready);
    /* Failed EP DMA must not publish its payload as native host PCM. */
    mcpx_apu_dsp_write(d, false, NV_PAPU_EPRST, 3, 4);
    for (unsigned i = 0; i < 7; i++)
        mcpx_apu_dsp_write(d, false, (0x20 + i) * 4, ep_node[i], 4);
    d->regs[NV_PAPU_EPFMAXSGE] = 0;
    d->regs[NV_PAPU_EPOFCUR0] = 0x17fc;
    memset(d->monitor.frame_buf, 0x5a, sizeof(d->monitor.frame_buf));
    run_locked_frame(d, mixbins);
    CHECK(d->ep.dsp->dma.error && !d->ep.dsp->dma.eol);
    CHECK(d->monitor.ep_pcm_offset == 0);
    for (unsigned i = 0; i < sizeof(d->monitor.frame_buf); i++)
        CHECK(((uint8_t *)d->monitor.frame_buf)[i] == 0x5a);
    CHECK(d->regs[NV_PAPU_EPOFCUR0] == 0x17fc);
    const char *capture = getenv("RECOMP_DSP_FRAME_DUMP");
    if (capture && *capture) {
        check_capture(capture, "gp");
        check_capture(capture, "ep");
        if (getenv("DSP_TEST_CAPTURE_LEGACY")) {
            char path[1024];
            snprintf(path, sizeof(path), "%s.gp.frame.bin", capture);
            FILE *file = fopen(path, "r+b");
            uint32_t legacy_version = 1;
            CHECK(file && fseek(file, 4, SEEK_SET) == 0);
            CHECK(fwrite(&legacy_version, sizeof(legacy_version), 1, file) == 1);
            CHECK(fclose(file) == 0);
        }
    }
    /* GP-only output must queue behind a mixed frame retained by the host. */
    mcpx_apu_dsp_write(d, false, NV_PAPU_EPRST, 0, 4);
    mcpx_apu_dsp_write(d, true, NV_PAPU_GPRST, 0, 4);
    mcpx_apu_dsp_write(d, true, NV_PAPU_GPRST, 3, 4);
    mcpx_apu_dsp_write(d, true, NV_PAPU_GPPMEM, 0x44f400, 4);
    mcpx_apu_dsp_write(d, true, NV_PAPU_GPPMEM + 4, 1, 4);
    mcpx_apu_dsp_write(d, true, NV_PAPU_GPPMEM + 8, 0x08c404, 4);
    memset(mixbins, 0, sizeof(mixbins));
    for (unsigned i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        mixbins[0][i] = 0.25f;
        mixbins[1][i] = -0.5f;
    }
    memset(d->monitor.frame_buf, 0x55, sizeof(d->monitor.frame_buf));
    d->monitor.frame_mixed = true;
    for (unsigned i = 0; i < 8; i++) {
        d->ep_frame_div = i;
        d->gp.dsp->core.pc = 0;
        run_locked_frame(d, mixbins);
        CHECK(d->gp.dsp->core.is_idle);
        for (unsigned byte = 0; byte < 1024; byte++)
            CHECK(((uint8_t *)d->monitor.frame_buf)[byte] == 0x55);
    }
    CHECK(d->monitor.ep_pcm_queued == 1024);
    mcpx_apu_dsp_pcm_consumed(d);
    CHECK(!d->monitor.frame_mixed && d->monitor.ep_pcm_ready && !d->monitor.ep_pcm_queued);
    for (unsigned i = 0; i < 256; i++)
        CHECK(d->monitor.frame_buf[i][0] == 8192 && d->monitor.frame_buf[i][1] == -16384);
    uint64_t gp_drops_before = d->monitor.ep_pcm_dropped_frames;
    unsigned gp_instructions_before = d->gp.dsp->core.num_inst;
    for (unsigned frame = 0; frame < 70; frame++)
        for (unsigned slice = 0; slice < 8; slice++) {
            d->ep_frame_div = slice;
            d->gp.dsp->core.pc = 0;
            run_locked_frame(d, mixbins);
        }
    CHECK(d->monitor.ep_pcm_queued == 64 * 1024 && d->monitor.ep_pcm_capacity == 64 * 1024);
    CHECK(d->monitor.ep_pcm_dropped_frames == gp_drops_before + 6);
    CHECK(d->gp.dsp->core.num_inst > gp_instructions_before && !d->gp.dsp->dma.error && !d->gp.dsp->dma.eol);
    for (unsigned i = 0; i < 256; i++)
        CHECK(d->monitor.frame_buf[i][0] == 8192 && d->monitor.frame_buf[i][1] == -16384);
    free(d->monitor.ep_pcm_queue);
    d->monitor.ep_pcm_queue = NULL;
    d->monitor.ep_pcm_queued = d->monitor.ep_pcm_capacity = 0;
    dsp_test_fail_alloc = 1;
    for (unsigned slice = 0; slice < 8; slice++) {
        d->ep_frame_div = slice;
        d->gp.dsp->core.pc = 0;
        run_locked_frame(d, mixbins);
    }
    dsp_test_fail_alloc = 0;
    CHECK(d->monitor.ep_pcm_dropped_frames == gp_drops_before + 7);
    CHECK(!d->monitor.ep_pcm_queued && !d->monitor.ep_pcm_capacity && !d->monitor.ep_pcm_queue);
    CHECK(!d->gp.dsp->dma.error && !d->gp.dsp->dma.eol);
    for (unsigned i = 0; i < 256; i++)
        CHECK(d->monitor.frame_buf[i][0] == 8192 && d->monitor.frame_buf[i][1] == -16384);
    /* A continuously running guest permits reset and application shutdown. */
    for (unsigned shutdown = 0; shutdown < 2; shutdown++) {
    mcpx_apu_dsp_write(d, false, NV_PAPU_EPRST, 0, 4);
    mcpx_apu_dsp_write(d, true, NV_PAPU_GPRST, 0, 4);
    mcpx_apu_dsp_write(d, true, NV_PAPU_GPRST, 3, 4);
    if (shutdown) {
        mcpx_apu_dsp_write(d, false, NV_PAPU_EPRST, 3, 4);
        mcpx_apu_dsp_write(d, false, NV_PAPU_GPPMEM, 0x0c0000, 4);
    }
    mcpx_apu_dsp_write(d, true, NV_PAPU_GPPMEM, 0x0c0000, 4); /* JMP 0 */
    HANDLE thread = CreateThread(NULL, 0, looping_frame, d, 0, NULL);
    CHECK(thread);
    bool running = false;
    for (unsigned attempt = 0; attempt < 1000 && !running; attempt++) {
        qemu_mutex_lock(&d->lock);
        running = d->gp.dsp->core.cycle_count >= 1000 && !d->gp.dsp->core.is_idle;
        qemu_mutex_unlock(&d->lock);
        if (!running) Sleep(1);
    }
    CHECK(running);
    if (shutdown) {
        qemu_mutex_lock(&d->lock);
        qatomic_set(&d->exiting, true);
        qemu_mutex_unlock(&d->lock);
    } else {
        mcpx_apu_dsp_write(d, true, NV_PAPU_GPRST, 0, 4);
    }
    CHECK(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0);
    CHECK(!d->gp.dsp->dma.eol);
    if (shutdown) {
        CHECK(d->gp.dsp->core.cycle_count >= 1000 && !d->gp.dsp->core.is_idle);
        CHECK(d->ep.dsp->core.cycle_count == 0 && !d->ep.dsp->dma.eol);
    } else {
        CHECK(d->gp.dsp->core.cycle_count == 0);
    }
    CloseHandle(thread);
    }
    free(d->gp.dsp);
    free(d->ep.dsp);
    free(d->monitor.ep_pcm_queue);
    free(d);
    puts("Guest DSP bootstrap, MMIO, DMA command completion and native PCM frame passed");
    return 0;
}
