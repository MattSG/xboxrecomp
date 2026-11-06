#include "interp/dsp_cpu.h"
#include "dsp_dma.h"
#include "dsp_dma_regs.h"
#ifndef DSP_REFERENCE_CPU_ONLY
#include "dsp.h"
#endif
#include <stdlib.h>
#include <string.h>

/* Keep checks active in Release builds. */
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "DSP check failed at line %d: %s\n", __LINE__, #condition); \
    exit(1); } } while (0)

static uint32_t polling_status;
static unsigned polling_reads;
static uint32_t polling_peripheral_read(dsp_core_t *core, uint32_t address)
{
    (void)core;
    CHECK(address == 0xffffd6);
    polling_reads++;
    return polling_status;
}

static void check_polling_instructions(dsp_core_t *core)
{
    /* Same uploaded words and peripheral inputs in port and upstream builds.
     * These inputs test CPU branch semantics, not the DMA engine's timing. */
    core->read_peripheral = polling_peripheral_read;
    dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0cd604); /* brclr #4,x:$ffffd6 */
    dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0); /* branch to itself */
    dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x0cd624); /* brset #4,x:$ffffd6 */
    dsp56k_write_memory(core, DSP_SPACE_P, 0x103, 0);
    core->pc = 0x100;
    polling_status = 0;
    dsp56k_execute_instruction(core);
    CHECK(core->pc == 0x100 && polling_reads == 1);
    polling_status = DMA_CONTROL_RUNNING;
    dsp56k_execute_instruction(core);
    CHECK(core->pc == 0x102 && polling_reads == 2);
    dsp56k_execute_instruction(core);
    CHECK(core->pc == 0x102 && polling_reads == 3);
    polling_status = DMA_CONTROL_STOPPED;
    dsp56k_execute_instruction(core);
    CHECK(core->pc == 0x104 && polling_reads == 4);
    core->read_peripheral = NULL;
}

#ifndef DSP_REFERENCE_CPU_ONLY
static uint32_t bit_value, bit_address;
static unsigned bit_reads, bit_writes;
static uint32_t bit_read(dsp_core_t *core, uint32_t address)
{
    (void)core;
    CHECK(address == bit_address);
    bit_reads++;
    return bit_value;
}
static void bit_write(dsp_core_t *core, uint32_t address, uint32_t value)
{
    (void)core;
    CHECK(address == bit_address);
    bit_writes++;
    bit_value = value;
}
static void check_compatibility_peripheral_window(dsp_core_t *core)
{
    core->read_peripheral = bit_read;
    core->write_peripheral = bit_write;
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned offset = 0; offset < 128; offset++) {
        core->registers[DSP_REG_SR] = compatibility << DSP_SR_SC;
        bit_address = 0xffff80 + offset;
        uint32_t address = compatibility ? 0xff80 + offset : bit_address;
        bit_reads = bit_writes = 0;
        bit_value = 0x123456;
        CHECK(dsp56k_read_memory(core, DSP_SPACE_X, address) == bit_value);
        dsp56k_write_memory(core, DSP_SPACE_X, address, 0x654321);
        CHECK(bit_reads == 1 && bit_writes == 1 && bit_value == 0x654321);
        /* Uploaded MOVE X0,X:(R0), then MOVE X:(R0),X0. */
        core->registers[DSP_REG_R0] = address;
        core->registers[DSP_REG_X0] = 0xabcdef;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x446000);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x44e000);
        dsp56k_execute_instruction(core);
        CHECK(bit_writes == 2 && bit_value == 0xabcdef);
        core->registers[DSP_REG_X0] = 0;
        dsp56k_execute_instruction(core);
        CHECK(bit_reads == 2 && core->registers[DSP_REG_X0] == 0xabcdef);
        CHECK(core->pc == 0x102 && core->registers[DSP_REG_R0] == address);
    }
    core->registers[DSP_REG_SR] = 0;
    bit_reads = 0;
    CHECK(dsp56k_read_memory(core, DSP_SPACE_X, 0xff80) == 0xffffff);
    CHECK(bit_reads == 0);
    core->read_peripheral = NULL;
    core->write_peripheral = NULL;
}

static void check_lower_io_register_moves(dsp_core_t *core)
{
    core->read_peripheral = bit_read;
    core->write_peripheral = bit_write;
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned reg = DSP_REG_R0; reg <= DSP_REG_M7; reg++)
    for (unsigned address = 0; address < 64; address++)
    for (unsigned store = 0; store < 2; store++) {
        core->registers[DSP_REG_SR] = compatibility << DSP_SR_SC;
        core->registers[reg] = 0xabcdef;
        bit_address = 0xffff80 + address;
        bit_value = 0x123456;
        bit_reads = bit_writes = 0;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           0x044080 | store << 15 | reg << 8 |
                           (address & 0x20) << 1 | (address & 0x1f));
        dsp56k_execute_instruction(core);
        uint32_t mask = compatibility ? 0xffff : 0xffffff;
        CHECK(bit_reads == !store && bit_writes == store);
        CHECK(bit_value == (store ? 0xabcdef & mask : 0x123456));
        CHECK(core->registers[reg] == (store ? 0xabcdef : 0x123456 & mask));
        CHECK(core->registers[DSP_REG_SR] == compatibility << DSP_SR_SC);
        CHECK(core->pc == 0x101);
    }
    core->read_peripheral = NULL;
    core->write_peripheral = NULL;
}

static void check_lower_io_program_moves(dsp_core_t *core)
{
    const unsigned modes[] = {0, 1, 2, 3, 4, 5, 7};
    core->read_peripheral = bit_read;
    core->write_peripheral = bit_write;
    for (unsigned reg = 0; reg < 8; reg++)
    for (unsigned m = 0; m < 7; m++)
    for (unsigned address = 0; address < 64; address++)
    for (unsigned store = 0; store < 2; store++) {
        unsigned mode = modes[m];
        uint32_t target = mode == 5 ? 0x23 : mode == 7 ? 0x1f : 0x20;
        uint32_t updated = mode == 0 ? 0x1d : mode == 1 ? 0x23 :
                           mode == 2 || mode == 7 ? 0x1f : mode == 3 ? 0x21 : 0x20;
        core->registers[DSP_REG_SR] = 0;
        core->registers[DSP_REG_R0 + reg] = 0x20;
        core->registers[DSP_REG_N0 + reg] = 3;
        core->registers[DSP_REG_M0 + reg] = 0xffffff;
        dsp56k_write_memory(core, DSP_SPACE_P, target, 0xabcdef);
        bit_address = 0xffff80 + address;
        bit_value = 0x123456;
        bit_reads = bit_writes = 0;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           0x008000 | store << 14 | (mode * 8 + reg) << 8 | address);
        dsp56k_execute_instruction(core);
        CHECK(bit_reads == !store && bit_writes == store);
        CHECK(bit_value == (store ? 0xabcdef : 0x123456));
        CHECK(dsp56k_read_memory(core, DSP_SPACE_P, target) == (store ? 0xabcdef : 0x123456));
        CHECK(core->registers[DSP_REG_R0 + reg] == updated);
        CHECK(core->registers[DSP_REG_SR] == 0 && core->pc == 0x101);
    }
    core->read_peripheral = NULL;
    core->write_peripheral = NULL;
}

static void check_lower_io_program_upload(dsp_core_t *core)
{
    core->read_peripheral = bit_read;
    core->write_peripheral = bit_write;
    for (unsigned address = 0; address < 64; address++) {
        core->registers[DSP_REG_SR] = 0;
        core->registers[DSP_REG_A0] = core->registers[DSP_REG_A1] = core->registers[DSP_REG_A2] = 0;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x20, 0x014180);
        core->pc = 0x20;
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_A1] == 1);
        bit_address = 0xffff80 + address;
        bit_value = 0x014284; /* Replace previously decoded ADD with SUB. */
        bit_reads = bit_writes = 0;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x00b000 | address);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x20);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x102 && bit_reads == 1 && bit_writes == 0);
        CHECK(dsp56k_read_memory(core, DSP_SPACE_P, 0x20) == 0x014284);
        core->pc = 0x20;
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_A1] == 0xffffff && core->registers[DSP_REG_A2] == 0xff);
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x00f000 | address);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x102 && bit_reads == 1 && bit_writes == 1);
        CHECK(bit_value == 0x014284);
    }
    core->read_peripheral = NULL;
    core->write_peripheral = NULL;
}

static void check_lower_io_bits(dsp_core_t *core)
{
    /* DSP56300FM: both short I/O banks, all X addresses/valid bits. */
    const uint32_t opcodes[2][4] = {{0x014000, 0x010000, 0x010020, 0x014020},
                                  {0x0b8000, 0x0a8000, 0x0a8020, 0x0b8020}};
    core->read_peripheral = bit_read;
    core->write_peripheral = bit_write;
    for (unsigned bank = 0; bank < 2; bank++)
    for (unsigned op = 0; op < 4; op++)
    for (unsigned addr = 0; addr < 64; addr++)
    for (unsigned bit = 0; bit < 24; bit++)
    for (unsigned set = 0; set < 2; set++) {
        uint32_t mask = 1u << bit;
        uint32_t original = set ? 0xa55a5a | mask : 0xa55a5a & ~mask;
        bit_value = original;
        bit_address = (bank ? 0xffffc0 : 0xffff80) + addr;
        bit_reads = bit_writes = 0;
        core->pc = 0;
        core->registers[DSP_REG_SR] = 0x1234;
        dsp56k_write_memory(core, DSP_SPACE_P, 0, opcodes[bank][op] | addr << 8 | bit);
        dsp56k_execute_instruction(core);
        uint32_t expected = op == 0 ? original ^ mask : op == 1 ? original & ~mask :
                            op == 2 ? original | mask : original;
        CHECK(core->pc == 1 && bit_reads == 1 && bit_writes == (op != 3));
        CHECK(bit_value == expected);
        CHECK(core->registers[DSP_REG_SR] == ((0x1234 & ~(1u << DSP_SR_C)) | set << DSP_SR_C));
    }
    core->read_peripheral = NULL;
    core->write_peripheral = NULL;
}
static void check_io_bit_branches(dsp_core_t *core)
{
    const uint32_t opcodes[3] = {0x048000, 0x0cc000, 0x0c8080};
    const uint32_t displacements[] = {0, 4, 0xfffffc, 0xffff00};
    core->read_peripheral = bit_read;
    core->write_peripheral = bit_write;
    for (unsigned bank = 0; bank < 3; bank++)
    for (unsigned space = 0; space < (bank == 2 ? 2u : 1u); space++)
    for (unsigned call = 0; call < 2; call++)
    for (unsigned branch_set = 0; branch_set < 2; branch_set++)
    for (unsigned addr = 0; addr < 64; addr++)
    for (unsigned bit = 0; bit < 24; bit++)
    for (unsigned set = 0; set < 2; set++)
    for (unsigned disp = 0; disp < sizeof(displacements) / sizeof(displacements[0]); disp++) {
        bit_value = set ? 1u << bit : 0;
        bit_address = (bank ? 0xffffc0 : 0xffff80) + addr;
        bit_reads = bit_writes = 0;
        if (bank == 2) {
            core->xram[addr] = bit_value;
            core->yram[addr] = bit_value;
        }
        core->pc = 0x100;
        core->registers[DSP_REG_SP] = 0;
        core->registers[DSP_REG_SR] = 0x1234;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, (opcodes[bank] | (call ? (bank ? 0x10000 : 0x80) : 0)) | addr << 8 | branch_set << 5 | space << 6 | bit);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, displacements[disp]);
        dsp56k_execute_instruction(core);
        uint32_t expected = set == branch_set ? (0x100 + displacements[disp]) & 0xffffff : 0x102;
        CHECK(core->pc == expected && bit_reads == (bank != 2) && bit_writes == 0);
        if (bank == 2) CHECK(core->xram[addr] == bit_value && core->yram[addr] == bit_value);
        CHECK(core->registers[DSP_REG_SR] == 0x1234);
        bool taken_call = call && set == branch_set;
        CHECK(core->registers[DSP_REG_SP] == (taken_call ? 1u : 0u));
        if (taken_call) {
            dsp56k_write_memory(core, DSP_SPACE_P, expected, 0x00000c);
            dsp56k_execute_instruction(core);
            CHECK(core->pc == 0x102 && core->registers[DSP_REG_SP] == 0);
        }
    }
    core->read_peripheral = NULL;
    core->write_peripheral = NULL;
}
static uint8_t scratch[64];
static void check_io_bit_jumps(dsp_core_t *core)
{
    const uint32_t opcodes[6][4] = {{0x018080, 0x0180a0, 0x01c080, 0x01c0a0},
                                   {0x0a8080, 0x0a80a0, 0x0b8080, 0x0b80a0},
                                   {0x0a0080, 0x0a00a0, 0x0b0080, 0x0b00a0},
                                   {0x0a00c0, 0x0a00e0, 0x0b00c0, 0x0b00e0},
                                   {0x0a6080, 0x0a60a0, 0x0b6080, 0x0b60a0},
                                   {0x0a60c0, 0x0a60e0, 0x0b60c0, 0x0b60e0}};
    core->read_peripheral = bit_read;
    for (unsigned bank = 0; bank < 6; bank++)
    for (unsigned address = 0; address < 64; address++)
    for (unsigned bit = 0; bit < 24; bit++)
    for (unsigned op = 0; op < 4; op++)
    for (unsigned set = 0; set < 2; set++) {
        bit_address = (bank ? 0xffffc0 : 0xffff80) + address;
        bit_value = set << bit;
        bit_reads = 0;
        if (bank >= 2) dsp56k_write_memory(core, (bank - 2) & 1, address, bit_value);
        core->registers[DSP_REG_SR] = 0x1234;
        core->registers[DSP_REG_SP] = 0;
        core->registers[DSP_REG_R0] = address;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, opcodes[bank][op] | (bank < 4 ? address << 8 : 0) | bit);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x200);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x200, 0x00000c); /* RTS */
        dsp56k_execute_instruction(core);
        bool taken = set == (op & 1);
        CHECK(core->pc == (taken ? 0x200 : 0x102));
        CHECK(bit_reads == (bank < 2 ? 1u : 0u) && bit_value == set << bit);
        if (bank >= 2) CHECK(dsp56k_read_memory(core, (bank - 2) & 1, address) == bit_value);
        CHECK(core->registers[DSP_REG_SR] == 0x1234);
        CHECK(core->registers[DSP_REG_R0] == address);
        CHECK(core->registers[DSP_REG_SP] == (taken && op >= 2 ? 1u : 0u));
        if (taken && op >= 2) {
            dsp56k_execute_instruction(core);
            CHECK(core->pc == 0x102 && core->registers[DSP_REG_SP] == 0);
        }
    }
    core->read_peripheral = NULL;
}

static void check_register_branches(dsp_core_t *core)
{
    const uint32_t offsets[] = {0, 1, 4, 0xfffffc, 0xffff00, 0xffffff, 0x7fffff, 0x800000};
    for (unsigned reg = 0; reg < 8; reg++)
    for (unsigned o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++) {
        uint32_t target = (0x100 + offsets[o]) & 0xffffff;
        core->registers[DSP_REG_R0 + reg] = offsets[o];
        for (unsigned call = 0; call < 2; call++) {
            core->pc = 0x100;
            core->registers[DSP_REG_SR] = 0x1234;
            core->registers[DSP_REG_SP] = 0;
            core->interrupt_state = DSP_INTERRUPT_NONE;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                               (call ? 0x0d1880 : 0x0d18c0) | reg << 8);
            dsp56k_execute_instruction(core);
            CHECK(core->pc == target && core->registers[DSP_REG_R0 + reg] == offsets[o]);
            CHECK(core->registers[DSP_REG_SR] == 0x1234);
            CHECK(core->registers[DSP_REG_SP] == call);
            if (call && target < DSP_PRAM_SIZE) {
                dsp56k_write_memory(core, DSP_SPACE_P, target, 0x00000c); /* RTS */
                dsp56k_execute_instruction(core);
                CHECK(core->pc == 0x101 && core->registers[DSP_REG_SP] == 0);
            }
        }
        /* DSP56300FM tables 12-17/12-18 supply an independent CCR oracle. */
        for (unsigned cc = 0; cc < 16; cc++)
        for (unsigned flags = 0; flags < 256; flags++) {
            core->pc = 0x100;
            core->registers[DSP_REG_SR] = flags;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0d1040 | cc);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x101, offsets[o]);
            dsp56k_execute_instruction(core);
            bool c = (flags & (1u << DSP_SR_C)) != 0;
            bool v = (flags & (1u << DSP_SR_V)) != 0;
            bool z = (flags & (1u << DSP_SR_Z)) != 0;
            bool n = (flags & (1u << DSP_SR_N)) != 0;
            bool u = (flags & (1u << DSP_SR_U)) != 0;
            bool e = (flags & (1u << DSP_SR_E)) != 0;
            bool l = (flags & (1u << DSP_SR_L)) != 0;
            const bool conditions[] = {!c, n == v, !z, !n,
                                       !(z || (!u && !e)), !e, !l, !z && n == v};
            bool taken = conditions[cc & 7] != ((cc & 8) != 0);
            CHECK(core->pc == (taken ? target : 0x102));
            core->pc = 0x100;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0d1840 | reg << 8 | cc);
            dsp56k_execute_instruction(core);
            CHECK(core->pc == (taken ? target : 0x101));
            CHECK(core->registers[DSP_REG_SR] == flags);
            CHECK(core->registers[DSP_REG_R0 + reg] == offsets[o]);
            /* Conditional subroutine forms use the same branch condition. */
            for (unsigned form = 0; form < 3; form++) {
                if (form == 2 && o >= 6) continue; /* Short form: signed nine-bit offsets. */
                core->pc = 0x100;
                core->registers[DSP_REG_SP] = 0;
                core->interrupt_state = DSP_INTERRUPT_NONE;
                uint32_t short_offset = offsets[o] & 0x1ff;
                uint32_t instruction = form == 0 ? 0x0d1000 | cc :
                    form == 1 ? 0x0d1800 | reg << 8 | cc :
                    0x050000 | cc << 12 | (short_offset & 31) | (short_offset & 0x1e0) << 1;
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100, instruction);
                dsp56k_write_memory(core, DSP_SPACE_P, 0x101, offsets[o]);
                dsp56k_execute_instruction(core);
                uint32_t next = form == 0 ? 0x102 : 0x101;
                CHECK(core->pc == (taken ? target : next));
                CHECK(core->registers[DSP_REG_SP] == (taken ? 1u : 0u));
                CHECK(core->registers[DSP_REG_SR] == flags);
                CHECK(core->registers[DSP_REG_R0 + reg] == offsets[o]);
                if (taken && target < DSP_PRAM_SIZE) {
                    dsp56k_write_memory(core, DSP_SPACE_P, target, 0x00000c);
                    dsp56k_execute_instruction(core);
                    CHECK(core->pc == next && core->registers[DSP_REG_SP] == 0);
                }
            }
        }
    }
}

static void check_register_bit_calls(dsp_core_t *core)
{
    const uint32_t offsets[] = {0, 4, 0xfffffc, 0xffff00};
    for (unsigned reg = DSP_REG_X0; reg <= DSP_REG_M7; reg++)
    for (unsigned bit = 0; bit < 24; bit++)
    for (unsigned set = 0; set < 2; set++)
    for (unsigned branch_set = 0; branch_set < 2; branch_set++)
    for (unsigned call = 0; call < 2; call++)
    for (unsigned o = 0; o < 4; o++) {
        unsigned width = reg == DSP_REG_A2 || reg == DSP_REG_B2 ? 8 : reg >= DSP_REG_R0 ? 16 : 24;
        uint32_t value = set && bit < width ? 1u << bit : 0;
        bool accumulator = reg == DSP_REG_A || reg == DSP_REG_B;
        if (accumulator) {
            unsigned which = reg & 1;
            core->registers[DSP_REG_A0 + which] = 0x123456;
            core->registers[DSP_REG_A1 + which] = value;
            core->registers[DSP_REG_A2 + which] = value & 0x800000 ? 0xff : 0;
        } else core->registers[reg] = value;
        core->registers[DSP_REG_SR] = 0x100;
        core->registers[DSP_REG_SP] = 0;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           (call ? 0x0dc080 : 0x0cc080) | reg << 8 | branch_set << 5 | bit);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, offsets[o]);
        dsp56k_execute_instruction(core);
        bool taken = ((value >> bit) & 1) == branch_set;
        uint32_t target = (0x100 + offsets[o]) & 0xffffff;
        CHECK(core->pc == (taken ? target : 0x102));
        CHECK(core->registers[DSP_REG_SP] == (taken && call ? 1u : 0u));
        CHECK(core->registers[DSP_REG_SR] == 0x100);
        CHECK(core->registers[accumulator ? DSP_REG_A1 + (reg & 1) : reg] == value);
        if (taken && call) {
            dsp56k_write_memory(core, DSP_SPACE_P, target, 0x00000c);
            dsp56k_execute_instruction(core);
            CHECK(core->pc == 0x102 && core->registers[DSP_REG_SP] == 0);
        }
    }
}

static void check_immediate_shifts(dsp_core_t *core)
{
    const uint32_t values[] = {0, 1, 0x800000, 0xffffff, 0xa55a5a};
    const unsigned sources[] = {DSP_REG_NULL, DSP_REG_A1, DSP_REG_B1,
                                DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1};
    for (unsigned form = 0; form < 7; form++)
    for (unsigned right = 0; right < 2; right++)
    for (unsigned accumulator = 0; accumulator < 2; accumulator++)
    for (unsigned count = 0; count <= 24; count++)
    for (unsigned i = 0; i < 5; i++)
    for (unsigned flags = 0; flags < 256; flags++) {
        unsigned reg = accumulator ? DSP_REG_B1 : DSP_REG_A1;
        core->registers[reg] = values[i];
        core->registers[accumulator ? DSP_REG_B0 : DSP_REG_A0] = 0x123456;
        core->registers[accumulator ? DSP_REG_B2 : DSP_REG_A2] = 0x55;
        uint32_t source_value = 0xabcd00 | count;
        if (form) core->registers[sources[form]] = source_value;
        uint32_t input = core->registers[reg];
        core->registers[DSP_REG_SR] = 0x100 | flags;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           (form ? (right ? 0x0c1e30 : 0x0c1e10) | (form + 1) << 1 :
                                   (right ? 0x0c1ec0 : 0x0c1e80) | count << 1) | accumulator);
        dsp56k_execute_instruction(core);
        uint32_t result = input, carry = 0;
        for (unsigned shift = 0; shift < count; shift++) {
            carry = right ? result & 1 : result >> 23;
            result = right ? result >> 1 : (result << 1) & 0xffffff;
        }
        uint32_t expected_sr = ((0x100 | flags) & ~15u) | carry |
                               ((result >> 23) << DSP_SR_N) | ((result == 0) << DSP_SR_Z);
        CHECK(core->registers[reg] == result && core->registers[DSP_REG_SR] == expected_sr);
        CHECK(core->registers[accumulator ? DSP_REG_B0 : DSP_REG_A0] == 0x123456);
        CHECK(core->registers[accumulator ? DSP_REG_B2 : DSP_REG_A2] == 0x55);
        CHECK(core->pc == 0x101);
        if (form && sources[form] != reg) CHECK(core->registers[sources[form]] == source_value);
    }
}

static void check_agu_register_loads(dsp_core_t *core)
{
    const uint32_t values[] = {0, 0xffff, 0x10000, 0x800000, 0xffffff};
    for (unsigned reg = DSP_REG_R0; reg <= DSP_REG_M7; reg++)
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned i = 0; i < 5; i++) {
        core->registers[DSP_REG_SR] = compatibility << DSP_SR_SC;
        core->pc = 0x100;
        uint32_t opcode = reg >= DSP_REG_M0 ? 0x05f420 | (reg & 31) :
            0x40f400 | (reg & 0x18) << 17 | (reg & 7) << 16;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, opcode);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, values[i]);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x102);
        CHECK(core->registers[reg] == (values[i] & (compatibility ? 0xffff : 0xffffff)));
        CHECK(core->registers[DSP_REG_SR] == compatibility << DSP_SR_SC);
    }
}

static void check_movec_agu_source(dsp_core_t *core)
{
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned reg = 0; reg < 8; reg++) {
        core->registers[DSP_REG_SR] = compatibility << DSP_SR_SC;
        core->registers[DSP_REG_M0 + reg] = 0xabcdef;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           0x0440a0 | DSP_REG_X0 << 8 | reg);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_X0] == (compatibility ? 0xcdef : 0xabcdef));
        CHECK(core->registers[DSP_REG_M0 + reg] == 0xabcdef);
        CHECK(core->pc == 0x101);
    }
}

static void check_move_agu_source(dsp_core_t *core)
{
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned reg = DSP_REG_R0; reg <= DSP_REG_M7; reg++) {
        core->registers[DSP_REG_SR] = compatibility << DSP_SR_SC;
        core->registers[reg] = 0xabcdef;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x071400 | reg);
        dsp56k_execute_instruction(core);
        CHECK(dsp56k_read_memory(core, DSP_SPACE_P, 0x14) == (compatibility ? 0xcdef : 0xabcdef));
        CHECK(core->registers[reg] == 0xabcdef);
        CHECK(core->pc == 0x101);
        if (reg <= DSP_REG_N7) {
            core->pc = 0x100;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                               0x200000 | reg << 13 | DSP_REG_X0 << 8);
            dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_X0] == (compatibility ? 0xcdef : 0xabcdef));
            CHECK(core->registers[reg] == 0xabcdef);
            CHECK(core->pc == 0x101);
        }
    }
}

static void check_parallel_memory_agu_source(dsp_core_t *core)
{
    const unsigned modes[] = {0, 1, 2, 3, 4, 5, 7};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned reg = DSP_REG_R0; reg <= DSP_REG_N7; reg++)
    for (unsigned space = 0; space < 2; space++)
    for (unsigned indirect = 0; indirect < 8; indirect++) {
        core->registers[DSP_REG_SR] = compatibility << DSP_SR_SC;
        core->registers[reg] = 0xabcdef;
        unsigned base_reg = (reg + 1) & 7;
        unsigned mode = indirect ? modes[indirect - 1] : 4;
        uint32_t address = mode == 5 ? 0x17 : mode == 7 ? 0x13 : 0x14;
        uint32_t updated = mode == 0 ? 0x11 : mode == 1 ? 0x17 :
                           mode == 2 || mode == 7 ? 0x13 : mode == 3 ? 0x15 : 0x14;
        if (indirect) {
            core->registers[DSP_REG_R0 + base_reg] = 0x14;
            core->registers[DSP_REG_N0 + base_reg] = 3;
            core->registers[DSP_REG_M0 + base_reg] = 0xffffff;
        }
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           0x400000 | (indirect ? 0x4000 | (mode * 8 + base_reg) << 8 : 0x1400) | (reg & 7) << 16 | (reg & 0x18) << 17 | space << 19);
        dsp56k_execute_instruction(core);
        CHECK(dsp56k_read_memory(core, space, address) == (compatibility ? 0xcdef : 0xabcdef));
        CHECK(core->registers[reg] == 0xabcdef);
        CHECK(core->pc == 0x101);
        CHECK(core->registers[DSP_REG_SR] == compatibility << DSP_SR_SC);
        if (indirect) CHECK(core->registers[DSP_REG_R0 + base_reg] == updated);
    }
}

static void check_relative_address_loads(dsp_core_t *core)
{
    const uint32_t offsets[] = {0, 1, 0xffff, 0x10000, 0x7fffff, 0xffff00};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned reg = 0; reg < 8; reg++)
    for (unsigned destination = 4; destination < 32; destination++)
    for (unsigned immediate = 0; immediate < 2; immediate++)
    for (unsigned n = 0; n < 6; n++) {
        core->registers[DSP_REG_R0 + reg] = offsets[n];
        core->registers[DSP_REG_SR] = 0x134 | compatibility << DSP_SR_SC;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           (immediate ? 0x044040 : 0x04c000 | reg << 8) | destination);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, offsets[n]);
        dsp56k_execute_instruction(core);
        uint32_t expected = (uint32_t)((uint64_t)0x100 + offsets[n]) % 0x1000000;
        if (destination == DSP_REG_A || destination == DSP_REG_B) {
            unsigned middle = destination == DSP_REG_A ? DSP_REG_A1 : DSP_REG_B1;
            unsigned low = destination == DSP_REG_A ? DSP_REG_A0 : DSP_REG_B0;
            unsigned high = destination == DSP_REG_A ? DSP_REG_A2 : DSP_REG_B2;
            CHECK(core->registers[middle] == expected && core->registers[low] == 0);
            CHECK(core->registers[high] == (expected & 0x800000 ? 0xff : 0));
        } else {
            if (destination == DSP_REG_A2 || destination == DSP_REG_B2) expected &= 0xff;
            else if (destination >= DSP_REG_R0 && compatibility) expected &= 0xffff;
            CHECK(core->registers[destination] == expected);
        }
        if (destination != DSP_REG_R0 + reg) CHECK(core->registers[DSP_REG_R0 + reg] == offsets[n]);
        CHECK(core->registers[DSP_REG_SR] == (0x134 | compatibility << DSP_SR_SC));
        CHECK(core->pc == (immediate ? 0x102 : 0x101));
    }
}

static void check_offset_address_loads(dsp_core_t *core)
{
    const uint32_t addresses[] = {0, 0xffff, 0x10000, 0x800000, 0xffffff};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned source = 0; source < 8; source++)
    for (unsigned destination = 0; destination < 8; destination++)
    for (unsigned offset_register = 0; offset_register < 2; offset_register++)
    for (unsigned encoded_offset = 0; encoded_offset < 128; encoded_offset++)
    for (unsigned a = 0; a < 5; a++) {
        unsigned target = (offset_register ? DSP_REG_N0 : DSP_REG_R0) + destination;
        core->registers[DSP_REG_R0 + source] = addresses[a];
        core->registers[DSP_REG_SR] = 0x134 | compatibility << DSP_SR_SC;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           0x040000 | (encoded_offset >> 4) << 11 | source << 8 |
                           (encoded_offset & 15) << 4 | offset_register << 3 | destination);
        dsp56k_execute_instruction(core);
        int64_t offset = encoded_offset < 64 ? encoded_offset : (int64_t)encoded_offset - 128;
        uint32_t expected = (uint32_t)((int64_t)addresses[a] + offset) &
                            (compatibility ? 0xffff : 0xffffff);
        CHECK(core->registers[target] == expected);
        if (target != DSP_REG_R0 + source) CHECK(core->registers[DSP_REG_R0 + source] == addresses[a]);
        CHECK(core->registers[DSP_REG_SR] == (0x134 | compatibility << DSP_SR_SC));
        CHECK(core->pc == 0x101);
    }
}

static void check_arithmetic_right_shifts(dsp_core_t *core)
{
    const uint64_t values[] = {0, 1, UINT64_C(0x7fffffffffffff),
                              UINT64_C(0x80000000000000), UINT64_C(0xffffffffffffff),
                              UINT64_C(0xa5123456789abc)};
    const unsigned counts[] = {DSP_REG_NULL, DSP_REG_A1, DSP_REG_B1,
                               DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1};
    for (unsigned form = 0; form < 7; form++)
    for (unsigned source = 0; source < 2; source++)
    for (unsigned destination = 0; destination < 2; destination++)
    for (unsigned count = 0; count <= 55; count++)
    for (unsigned v = 0; v < 6; v++) {
        unsigned sh = source ? DSP_REG_B2 : DSP_REG_A2;
        unsigned sm = source ? DSP_REG_B1 : DSP_REG_A1;
        unsigned sl = source ? DSP_REG_B0 : DSP_REG_A0;
        core->registers[sh] = (uint32_t)(values[v] >> 48);
        core->registers[sm] = (uint32_t)(values[v] >> 24) & 0xffffff;
        core->registers[sl] = (uint32_t)values[v] & 0xffffff;
        if (form) core->registers[counts[form]] = 0x123400 | count;
        uint64_t input = ((uint64_t)core->registers[sh] << 48) |
                         ((uint64_t)core->registers[sm] << 24) | core->registers[sl];
        core->registers[DSP_REG_SR] = 0x134 | (1u << DSP_SR_FV);
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           (form ? 0x0c1e60 | source << 4 | (form + 1) << 1 :
                                   0x0c1c00 | source << 7 | count << 1) | destination);
        dsp56k_execute_instruction(core);
        uint64_t expected = input;
        unsigned carry = 0;
        for (unsigned step = 0; step < count; step++) {
            carry = expected & 1;
            expected = (expected >> 1) | (expected & (UINT64_C(1) << 55));
        }
        unsigned dh = destination ? DSP_REG_B2 : DSP_REG_A2;
        unsigned dm = destination ? DSP_REG_B1 : DSP_REG_A1;
        unsigned dl = destination ? DSP_REG_B0 : DSP_REG_A0;
        CHECK(core->registers[dh] == (expected >> 48));
        CHECK(core->registers[dm] == ((expected >> 24) & 0xffffff));
        CHECK(core->registers[dl] == (expected & 0xffffff));
        CHECK((core->registers[DSP_REG_SR] & 3) == carry);
        CHECK(core->registers[DSP_REG_SR] & (1u << DSP_SR_FV));
        CHECK(core->pc == 0x101);
    }
}

static void check_arithmetic_left_shifts(dsp_core_t *core)
{
    const uint64_t values[] = {0, 1, UINT64_C(0x7fffffffffffff),
                              UINT64_C(0x80000000000000), UINT64_C(0xffffffffffffff),
                              UINT64_C(0xa5123456789abc)};
    const unsigned counts[] = {DSP_REG_NULL, DSP_REG_A1, DSP_REG_B1,
                               DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1};
    for (unsigned form = 0; form < 7; form++)
    for (unsigned sticky = 0; sticky < 2; sticky++)
    for (unsigned source = 0; source < 2; source++)
    for (unsigned destination = 0; destination < 2; destination++)
    for (unsigned count = 0; count <= 55; count++)
    for (unsigned v = 0; v < 6; v++) {
        unsigned sh = source ? DSP_REG_B2 : DSP_REG_A2;
        unsigned sm = source ? DSP_REG_B1 : DSP_REG_A1;
        unsigned sl = source ? DSP_REG_B0 : DSP_REG_A0;
        core->registers[sh] = (uint32_t)(values[v] >> 48);
        core->registers[sm] = (uint32_t)(values[v] >> 24) & 0xffffff;
        core->registers[sl] = (uint32_t)values[v] & 0xffffff;
        if (form) core->registers[counts[form]] = 0x123400 | count;
        uint64_t input = ((uint64_t)core->registers[sh] << 48) |
                         ((uint64_t)core->registers[sm] << 24) | core->registers[sl];
        core->registers[DSP_REG_SR] = 0x134 | (1u << DSP_SR_FV) | sticky << DSP_SR_L;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           (form ? 0x0c1e40 | source << 4 | (form + 1) << 1 :
                                   0x0c1d00 | source << 7 | count << 1) | destination);
        dsp56k_execute_instruction(core);
        uint64_t expected = input;
        unsigned carry = 0, overflow = 0;
        for (unsigned step = 0; step < count; step++) {
            carry = (unsigned)(expected >> 55);
            expected = (expected * 2) & UINT64_C(0xffffffffffffff);
            overflow |= carry != (expected >> 55);
        }
        unsigned dh = destination ? DSP_REG_B2 : DSP_REG_A2;
        unsigned dm = destination ? DSP_REG_B1 : DSP_REG_A1;
        unsigned dl = destination ? DSP_REG_B0 : DSP_REG_A0;
        CHECK(core->registers[dh] == (expected >> 48));
        CHECK(core->registers[dm] == ((expected >> 24) & 0xffffff));
        CHECK(core->registers[dl] == (expected & 0xffffff));
        CHECK((core->registers[DSP_REG_SR] & 3) == (carry | overflow << DSP_SR_V));
        CHECK(((core->registers[DSP_REG_SR] >> DSP_SR_L) & 1) == (sticky | overflow));
        CHECK(core->registers[DSP_REG_SR] & (1u << DSP_SR_FV));
        CHECK(core->pc == 0x101);
    }
}

static void check_status_register_writes(dsp_core_t *core)
{
    const uint32_t values[] = {0, 0x80, 0x4000, 0x10000, 0x20000,
                               0x200000, 0xc00000, 0xffffff};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned i = 0; i < 8; i++) {
        uint32_t previous = 0x230000 | compatibility << DSP_SR_SC;
        core->registers[DSP_REG_SR] = previous;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f439);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, values[i]);
        dsp56k_execute_instruction(core);
        uint32_t expected = compatibility ? (previous & 0xff0000) | (values[i] & 0xffff) : values[i];
        expected &= ~(1u << 18 | 1u << 12);
        CHECK(core->registers[DSP_REG_SR] == expected);
        CHECK(core->pc == 0x102);
    }
}

static void check_status_register_reads(dsp_core_t *core)
{
    for (unsigned compatibility = 0; compatibility < 2; compatibility++) {
        uint32_t initial = 0x230134 | compatibility << DSP_SR_SC;
        uint32_t expected = compatibility ? initial & 0xffff : initial;
        core->registers[DSP_REG_SR] = initial;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0444b9); /* MOVEC SR,X0 */
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_X0] == ((expected & 0xffff) << 8));
        CHECK(core->registers[DSP_REG_SR] == initial && core->pc == 0x101);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x071439); /* MOVEM SR,P:$14 */
        core->pc = 0x100;
        dsp56k_execute_instruction(core);
        CHECK(dsp56k_read_memory(core, DSP_SPACE_P, 0x14) == expected);
        CHECK(core->registers[DSP_REG_SR] == initial && core->pc == 0x101);
    }
}

static void check_loop_register_transfers(dsp_core_t *core)
{
    const uint32_t values[] = {0, 1, 0xffff, 0x10000, 0x800000, 0xffffff};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned reg = DSP_REG_LA; reg <= DSP_REG_LC; reg++)
    for (unsigned i = 0; i < 6; i++) {
        uint32_t initial_sr = 0x134 | compatibility << DSP_SR_SC;
        uint32_t expected = values[i] & (compatibility ? 0xffff : 0xffffff);
        core->registers[DSP_REG_SR] = initial_sr;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f420 | (reg & 31));
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, values[i]);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[reg] == expected && core->pc == 0x102);
        CHECK(core->registers[DSP_REG_SR] == initial_sr);
        core->registers[reg] = values[i];
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0444a0 | (reg & 31));
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_X0] == expected && core->pc == 0x101);
        CHECK(core->registers[reg] == values[i]);
        CHECK(core->registers[DSP_REG_SR] == initial_sr);
    }
}

static void check_wide_repeat_counts(dsp_core_t *core)
{
    const uint32_t counts[] = {0, 1, 2, 0xffff, 0x10001, 0x20001};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned i = 0; i < 6; i++) {
        uint32_t count = counts[i] & (compatibility ? 0xffff : 0xffffff);
        if (!count) count = 0x10000;
        core->registers[DSP_REG_SR] = compatibility << DSP_SR_SC;
        core->registers[DSP_REG_LC] = 0x123456;
        core->registers[DSP_REG_X0] = counts[i];
        core->registers[DSP_REG_A0] = core->registers[DSP_REG_A1] = core->registers[DSP_REG_A2] = 0;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x06c420); /* REP X0 */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x014180); /* ADD #1,A */
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x101 && core->registers[DSP_REG_LC] == count);
        for (unsigned iteration = 0; iteration < count; iteration++) {
            dsp56k_execute_instruction(core);
            CHECK(core->pc == (iteration + 1 == count ? 0x102 : 0x101));
        }
        CHECK(core->registers[DSP_REG_A1] == count);
        CHECK(core->registers[DSP_REG_LC] == 0x123456 && !core->loop_rep);
        CHECK(core->registers[DSP_REG_X0] == counts[i]);
    }
}

static void check_repeat_stack_source(dsp_core_t *core)
{
    core->registers[DSP_REG_SR] = 0;
    core->registers[DSP_REG_SP] = 0;
    core->registers[DSP_REG_LC] = 0x123456;
    core->registers[DSP_REG_A0] = core->registers[DSP_REG_A1] = core->registers[DSP_REG_A2] = 0;
    core->pc = 0x100;
    dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f43c); /* MOVEC #3,SSH */
    dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 3);
    dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x06fc20); /* REP SSH */
    dsp56k_write_memory(core, DSP_SPACE_P, 0x103, 0x014180);
    dsp56k_execute_instruction(core);
    CHECK(core->registers[DSP_REG_SP] == 1);
    dsp56k_execute_instruction(core);
    CHECK(core->registers[DSP_REG_SP] == 0 && core->registers[DSP_REG_LC] == 3);
    for (unsigned i = 0; i < 3; i++) dsp56k_execute_instruction(core);
    CHECK(core->pc == 0x104 && core->registers[DSP_REG_A1] == 3);
    CHECK(core->registers[DSP_REG_LC] == 0x123456 && !core->loop_rep);
}

static void check_loop_controller_sources(dsp_core_t *core)
{
    const unsigned sources[] = {DSP_REG_SP, DSP_REG_SSL, DSP_REG_SR, DSP_REG_LA, DSP_REG_LC};
    for (unsigned relative = 0; relative < 2; relative++)
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned source = 0; source < 5; source++) {
        uint32_t status = 1 | (compatibility << DSP_SR_SC);
        const uint32_t expected[] = {4, 6, status, 5, 6};
        core->registers[DSP_REG_SR] = status;
        core->registers[DSP_REG_SP] = 3;
        core->registers[DSP_REG_SSL] = 0x1234;
        core->registers[DSP_REG_LA] = 5;
        core->registers[DSP_REG_LC] = 6;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x06c000 | (sources[source] << 8) | (relative ? 0x10 : 0));
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, relative ? 0x10 : 0x110);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x00008c); /* ENDDO */
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_LC] == expected[source]);
        CHECK(core->registers[DSP_REG_LA] == 0x110 && core->registers[DSP_REG_SP] == 5);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x103 && core->registers[DSP_REG_SP] == 3);
        CHECK(core->registers[DSP_REG_LA] == 5 && core->registers[DSP_REG_LC] == 6);
        CHECK(core->registers[DSP_REG_SR] == status);
    }
}

static void check_wide_stack(dsp_core_t *core)
{
    const uint32_t values[] = {0, 0xffff, 0x10000, 0x123456, 0x800000, 0xffffff};
    for (unsigned i = 0; i < 6; i++) {
        core->registers[DSP_REG_SR] = 0;
        core->registers[DSP_REG_SP] = 0;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f43c); /* MOVEC #value,SSH */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, values[i]);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x05f43d); /* MOVEC #value,SSL */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x103, values[5-i]);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x104, 0x00000c); /* RTS */
        dsp56k_execute_instruction(core);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_SSH] == values[i] && core->stack[0][1] == values[i]);
        CHECK(core->registers[DSP_REG_SSL] == values[5-i] && core->stack[1][1] == values[5-i]);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == values[i] && core->registers[DSP_REG_SP] == 0);
        core->registers[DSP_REG_LA] = values[i];
        core->registers[DSP_REG_LC] = values[5-i];
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x060180); /* DO #1,$110 */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x110);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x00008c); /* ENDDO */
        dsp56k_execute_instruction(core);
        CHECK(core->stack[0][1] == values[i] && core->stack[1][1] == values[5-i]);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_LA] == values[i] && core->registers[DSP_REG_LC] == values[5-i]);
    }
}

static void check_interrupt_return_stack(dsp_core_t *core)
{
    const uint32_t addresses[] = {0, 0xffff, 0x10000, 0x123456, 0x800000, 0xffffff};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned i = 0; i < 6; i++) {
        uint32_t status = 0xc00380 | (compatibility << DSP_SR_SC);
        /* Construct a saved interrupt frame through actual guest writes. */
        core->registers[DSP_REG_SR] = 0;
        core->registers[DSP_REG_SP] = 0;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f43c);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, addresses[i]);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x05f43d);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x103, status);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x104, 0x000004); /* RTI */
        dsp56k_execute_instruction(core);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_SP] == 1);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == addresses[i]);
        CHECK(core->registers[DSP_REG_SR] == status);
        CHECK(core->registers[DSP_REG_SP] == 0);
    }
}

static void check_forever_loops(dsp_core_t *core)
{
    for (unsigned relative = 0; relative < 2; relative++)
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned count = 0; count < 3; count++) {
        uint32_t status = 0xc00380 | (compatibility << DSP_SR_SC);
        uint32_t mask = compatibility ? 0xffff : 0xffffff;
        core->registers[DSP_REG_SR] = status;
        core->registers[DSP_REG_SP] = 0;
        core->registers[DSP_REG_LA] = 0x55;
        core->registers[DSP_REG_LC] = count;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, relative ? 0x000202 : 0x000203);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, relative ? 2 : 0x102);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x000000); /* NOP */
        dsp56k_execute_instruction(core);
        for (unsigned i = 0; i < 5; i++) {
            dsp56k_execute_instruction(core);
            CHECK(core->pc == 0x102 && core->registers[DSP_REG_SP] == 2);
            CHECK(core->registers[DSP_REG_LC] == ((count - i - 1) & mask));
        }
        dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x00008c); /* ENDDO */
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x103 && core->registers[DSP_REG_SP] == 0);
        CHECK(core->registers[DSP_REG_LA] == 0x55 && core->registers[DSP_REG_LC] == count);
        CHECK(core->registers[DSP_REG_SR] == status);
    }
}

static void check_nested_loop_flags(dsp_core_t *core)
{
    for (unsigned relative = 0; relative < 2; relative++)
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned outer_forever = 0; outer_forever < 2; outer_forever++)
    for (unsigned inner_forever = 0; inner_forever < 2; inner_forever++) {
        uint32_t status = 0xc00380 | (compatibility << DSP_SR_SC);
        core->registers[DSP_REG_SR] = status;
        core->registers[DSP_REG_SP] = 0;
        core->registers[DSP_REG_LA] = 0x55;
        core->registers[DSP_REG_LC] = 1;
        core->registers[DSP_REG_A0] = core->registers[DSP_REG_A1] = core->registers[DSP_REG_A2] = 0;
        core->pc = 0x100;
        uint32_t forever = relative ? 0x000202 : 0x000203;
        uint32_t counted = relative ? 0x060290 : 0x060280;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, outer_forever ? forever : counted);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, relative ? 8 : 0x108);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x102, inner_forever ? forever : counted);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x103, relative ? (inner_forever ? 5 : 2) : (inner_forever ? 0x107 : 0x104));
        dsp56k_write_memory(core, DSP_SPACE_P, 0x104, inner_forever ? 0x00008c : 0x014180);
        for (unsigned address = 0x105; address <= 0x109; address++)
            dsp56k_write_memory(core, DSP_SPACE_P, address, 0);
        dsp56k_execute_instruction(core);
        for (unsigned iteration = 0; iteration < 2; iteration++) {
            dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_SP] == 4);
            CHECK(!!(core->registers[DSP_REG_SR] & (1u << DSP_SR_FV)) == inner_forever);
            dsp56k_execute_instruction(core);
            if (!inner_forever) dsp56k_execute_instruction(core);
            CHECK(core->pc == 0x105 && core->registers[DSP_REG_SP] == 2);
            CHECK(!!(core->registers[DSP_REG_SR] & (1u << DSP_SR_FV)) == outer_forever);
            CHECK(core->registers[DSP_REG_LA] == 0x108);
            for (unsigned i = 0; i < 4; i++) dsp56k_execute_instruction(core);
        }
        CHECK(core->registers[DSP_REG_A1] == (inner_forever ? 0u : 4u));
        if (outer_forever) {
            CHECK(core->pc == 0x102 && core->registers[DSP_REG_SP] == 2);
            CHECK(core->registers[DSP_REG_LC] == (compatibility ? 0xffffu : 0xffffffu));
            dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x00008c);
            dsp56k_execute_instruction(core);
        } else CHECK(core->pc == 0x109);
        CHECK(core->registers[DSP_REG_SP] == 0);
        CHECK(core->registers[DSP_REG_LA] == 0x55 && core->registers[DSP_REG_LC] == 1);
        CHECK((core->registers[DSP_REG_SR] & ~0x7fu) == status);
    }
}

static void check_conditional_loop_exit(dsp_core_t *core)
{
    for (unsigned forever = 0; forever < 2; forever++)
    for (unsigned zero = 0; zero < 2; zero++)
    for (unsigned equal = 0; equal < 2; equal++) {
        uint32_t status = 0xc00380 | (zero << DSP_SR_Z);
        core->registers[DSP_REG_SR] = status;
        core->registers[DSP_REG_SP] = 0;
        core->registers[DSP_REG_LA] = 0x55;
        core->registers[DSP_REG_LC] = 0x123456;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, forever ? 0x000203 : 0x060280);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x110);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x000210 | (equal ? 10 : 2));
        dsp56k_execute_instruction(core);
        dsp56k_execute_instruction(core);
        if (zero == equal) {
            CHECK(core->pc == 0x111 && core->registers[DSP_REG_SP] == 0);
            CHECK(core->registers[DSP_REG_LA] == 0x55 && core->registers[DSP_REG_LC] == 0x123456);
            CHECK(core->registers[DSP_REG_SR] == status);
        } else {
            CHECK(core->pc == 0x103 && core->registers[DSP_REG_SP] == 2);
            CHECK(core->registers[DSP_REG_LA] == 0x110);
        }
    }
}

static void check_control_byte_logic(dsp_core_t *core)
{
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned target = 0; target < 4; target++)
    for (unsigned is_or = 0; is_or < 2; is_or++)
    for (unsigned immediate = 0; immediate < 256; immediate++) {
        uint32_t sr = 0x230134 | compatibility << DSP_SR_SC;
        uint32_t omr = 0xabcd8f;
        unsigned shift = target == 0 || target == 3 ? 8 : 0;
        unsigned reg = target < 2 ? DSP_REG_SR : DSP_REG_OMR;
        uint32_t previous = target < 2 ? sr : omr;
        uint32_t expected = is_or ? previous | immediate << shift :
            previous & (~(0xffu << shift) | immediate << shift);
        expected &= target < 2 ? 0xfbefff : 0xffffdf;
        core->registers[DSP_REG_SR] = sr;
        core->registers[DSP_REG_OMR] = omr;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, (is_or ? 0xf8 : 0xb8) | immediate << 8 | target);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[reg] == expected && core->pc == 0x101);
        CHECK(core->registers[target < 2 ? DSP_REG_OMR : DSP_REG_SR] == (target < 2 ? omr : sr));
    }
    dsp56k_reset_cpu(core);
}

static void check_operating_mode_moves(dsp_core_t *core)
{
    const uint32_t values[] = {0, 0x20, 0x4000, 0x10000, 0x800000, 0xffffff};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned i = 0; i < 6; i++) {
        core->registers[DSP_REG_SR] = compatibility << DSP_SR_SC;
        core->registers[DSP_REG_OMR] = 0x230000;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f43a);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, values[i]);
        dsp56k_execute_instruction(core);
        uint32_t expected = (compatibility ? 0x230000 | (values[i] & 0xffff) : values[i]) & 0xffffdf;
        CHECK(core->registers[DSP_REG_OMR] == expected && core->pc == 0x102);
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0444ba);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_X0] == (compatibility ? expected & 0xffff : expected));
    }
    dsp56k_reset_cpu(core);
}

static void check_vector_register_moves(dsp_core_t *core)
{
    const uint32_t values[] = {0, 0xff, 0x100, 0xffff, 0x10000, 0x123456, 0x800000, 0xffffff};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned i = 0; i < 8; i++) {
        uint32_t status = compatibility << DSP_SR_SC;
        uint32_t expected = values[i] & (compatibility ? 0xff00 : 0xffff00);
        core->registers[DSP_REG_SR] = status;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f430);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, values[i]);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_VBA] == expected && core->pc == 0x102);
        core->registers[DSP_REG_VBA] = values[i] & 0xffff00;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0444b0); /* MOVEC VBA,X0 */
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_X0] == expected && core->pc == 0x101);
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x071430); /* MOVEM VBA,P:$14 */
        dsp56k_execute_instruction(core);
        CHECK(dsp56k_read_memory(core, DSP_SPACE_P, 0x14) == expected);
        CHECK(core->registers[DSP_REG_SR] == status && core->pc == 0x101);
    }
    dsp56k_reset_cpu(core);
}

static void check_vector_relocation(dsp_core_t *core)
{
    for (unsigned base = 0; base < DSP_PRAM_SIZE; base += 0x100) {
        dsp56k_reset_cpu(core);
        core->pc = 0x180;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x180, 0x05f430); /* MOVEC #long,VBA */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x181, base | 0xab);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x182, 6); /* TRAP */
        for (unsigned address = 0x183; address < 0x190; address++)
            dsp56k_write_memory(core, DSP_SPACE_P, address, 0);
        dsp56k_write_memory(core, DSP_SPACE_P, base + 8, 0x0d0080);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[0x30] == base);
        for (unsigned step = 0; core->pc != 0x80 && step < 9; step++)
            dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x80 && core->registers[DSP_REG_SP] == 1);
    }
    dsp56k_reset_cpu(core);
    core->registers[DSP_REG_VBA] = 0x300;
    core->registers[DSP_REG_LC] = 0x345;
    core->pc = 0x180;
    dsp56k_write_memory(core, DSP_SPACE_P, 0x180, 0x0603a0); /* REP #3 */
    dsp56k_write_memory(core, DSP_SPACE_P, 0x181, 0);
    for (unsigned step = 0; step < 4; step++) dsp56k_execute_instruction(core);
    CHECK(core->pc == 0x182 && core->registers[DSP_REG_LC] == 0x345);
    CHECK(core->registers[DSP_REG_VBA] == 0x300);
    dsp56k_reset_cpu(core);
    CHECK(core->registers[DSP_REG_VBA] == 0);
}

static void check_stop_execution(dsp_core_t *core)
{
    dsp56k_reset_cpu(core);
    core->pc = 0x100;
    dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x87); /* STOP */
    dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x014180);
    dsp56k_execute_instruction(core);
    CHECK(core->is_idle && core->pc == 0x101);
    uint32_t cycles = core->num_inst;
    dsp56k_add_interrupt(core, DSP_INTER_TRAP);
    dsp56k_execute_instruction(core);
    CHECK(core->is_idle && core->pc == 0x101 && core->num_inst == cycles);
    CHECK(core->registers[DSP_REG_A1] == 0 && core->interrupt_counter == 1);
    dsp56k_reset_cpu(core);
    core->pc = 0x101;
    dsp56k_execute_instruction(core);
    CHECK(core->registers[DSP_REG_A1] == 1);
}

static void check_wait_execution(dsp_core_t *core)
{
    for (unsigned pending = 0; pending < 2; pending++) {
        dsp56k_reset_cpu(core);
        core->pc = 0x100;
        core->registers[DSP_REG_SR] = 0x380;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x86); /* WAIT */
        for (unsigned address = 0x101; address < 0x110; address++)
            dsp56k_write_memory(core, DSP_SPACE_P, address, 0);
        dsp56k_write_memory(core, DSP_SPACE_P, 8, 0x0d0200);
        if (pending) dsp56k_add_interrupt(core, DSP_INTER_TRAP);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x101 && core->registers[DSP_REG_A1] == 0);
        if (!pending) {
            CHECK(core->is_idle);
            uint32_t cycles = core->num_inst;
            dsp56k_execute_instruction(core);
            CHECK(core->pc == 0x101 && core->num_inst == cycles);
            core->interrupt_ipl[DSP_INTER_TRAP] = 1;
            dsp56k_add_interrupt(core, DSP_INTER_TRAP);
            CHECK(core->is_idle); /* Masked request must not wake WAIT. */
            core->interrupt_is_pending[DSP_INTER_TRAP] = 0;
            core->interrupt_counter = 0;
            core->interrupt_ipl[DSP_INTER_TRAP] = 3;
            dsp56k_add_interrupt(core, DSP_INTER_TRAP);
        }
        CHECK(!core->is_idle);
        for (unsigned step = 0; core->pc != 0x200 && step < 8; step++)
            dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x200 && core->registers[DSP_REG_SP] == 1);
        CHECK(core->registers[DSP_REG_SSL] == 0x380);
    }
    dsp56k_reset_cpu(core);
}

static void check_disabled_cache_interrupts(dsp_core_t *core)
{
    const uint32_t instructions[] = {3, 1, 2, 0x0be081, 15, 0x0ae081, 14, 5};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned index = 0; index < sizeof(instructions)/sizeof(instructions[0]); index++) {
        dsp56k_reset_cpu(core);
        uint32_t status = 0xc00380 | compatibility << DSP_SR_SC;
        core->registers[DSP_REG_SR] = status; /* IPL 3 must not mask this exception. */
        core->registers[DSP_REG_R0] = 0x300;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, instructions[index]);
        for (unsigned address = 0x101; address < 0x110; address++)
            dsp56k_write_memory(core, DSP_SPACE_P, address, 0);
        dsp56k_write_memory(core, DSP_SPACE_P, 4, 0x0d0200); /* Illegal vector JSR $200 */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x200, 0x014180); /* Guest exception handler */
        for (unsigned step = 0; core->pc != 0x200 && step < 9; step++)
            dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x200 && core->registers[DSP_REG_SP] == 1);
        CHECK(core->registers[DSP_REG_SSL] == status);
        CHECK(core->registers[DSP_REG_R0] == 0x300);
        core->registers[DSP_REG_A0] = core->registers[DSP_REG_A1] = core->registers[DSP_REG_A2] = 0;
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_A1] == 1);
    }
    /* With SR[CE] set the cache is transparent: no exception, operands consumed. */
    const struct { uint32_t opcode; uint32_t length; uint32_t r0; } enabled[] = {
        {3, 1, 0x300}, {1, 1, 0x300}, {2, 1, 0x300}, {0x0bd881, 1, 0x301},
        {15, 2, 0x300}, {0x0ad881, 1, 0x301}, {0x0af081, 2, 0x300}, {14, 2, 0x300},
    };
    for (unsigned index = 0; index < sizeof(enabled)/sizeof(enabled[0]); index++) {
        dsp56k_reset_cpu(core);
        core->registers[DSP_REG_SR] = 0xc00300 | (1u << 19);
        core->registers[DSP_REG_R0] = 0x300;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, enabled[index].opcode);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x000040);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x100 + enabled[index].length);
        CHECK(core->registers[DSP_REG_R0] == enabled[index].r0);
        CHECK(core->registers[DSP_REG_SP] == 0 && !core->interrupt_counter);
    }
    dsp56k_reset_cpu(core);
}

static void check_stack_extension_registers(dsp_core_t *core)
{
    for (unsigned sc = 0; sc < 2; sc++) {
        for (unsigned i = 0; i < 2; i++) {
            unsigned reg = i ? DSP_REG_SZ : DSP_REG_EP;
            dsp56k_reset_cpu(core);
            core->registers[DSP_REG_SR] = sc << DSP_SR_SC;
            core->pc = 0x100;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f400 | reg);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0xabcdef);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x044480 | reg); /* MOVEC reg,X0. */
            dsp56k_execute_instruction(core);
            CHECK(core->registers[reg] == (sc ? 0xcdefu : 0xabcdefu));
            dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_X0] == core->registers[reg]);
            uint32_t saved_size = core->registers[DSP_REG_SZ];
            uint32_t saved_pointer = core->registers[DSP_REG_EP];
            dsp56k_reset_cpu(core);
            CHECK(core->registers[DSP_REG_SZ] == saved_size);
            CHECK(core->registers[DSP_REG_EP] == saved_pointer);
        }
    }
    core->registers[DSP_REG_SZ] = 0;
    dsp56k_reset_cpu(core);
}

static void check_stack_error_transition(dsp_core_t *core)
{
    for (unsigned old = 0; old < 4; old++) {
        for (unsigned next = 0; next < 4; next++) {
            dsp56k_reset_cpu(core);
            core->registers[DSP_REG_SP] = old << DSP_SP_SE;
            core->pc = 0x100;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f43b);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x101, (next << DSP_SP_SE) | 3);
            dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_SP] == ((next << DSP_SP_SE) | 3));
            CHECK((core->interrupt_pipeline_count != 0) ==
                  (!(old & 1) && (next & 1)));
        }
    }
    dsp56k_reset_cpu(core);
}

static void check_stack_fault_latching(dsp_core_t *core)
{
    for (unsigned uf = 0; uf < 2; uf++) {
        for (unsigned top = 0; top < 16; top++) {
            dsp56k_reset_cpu(core);
            uint32_t flags = (1u << DSP_SP_SE) | (uf << DSP_SP_UF);
            core->registers[DSP_REG_SP] = flags | top;
            core->registers[DSP_REG_SSH] = 0x123456;
            core->pc = 0x100;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0444bc); /* MOVEC SSH,X0 pops. */
            dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_SP] == (flags | ((top - 1) & 15)));
            CHECK(core->registers[DSP_REG_X0] == 0x123456);
            CHECK(core->interrupt_pipeline_count == 0);
        }
    }
    dsp56k_reset_cpu(core);
}

static void check_stack_move_width(dsp_core_t *core)
{
    for (unsigned sc = 0; sc < 2; sc++) {
        for (unsigned low = 0; low < 2; low++) {
            dsp56k_reset_cpu(core);
            unsigned reg = low ? DSP_REG_SSL : DSP_REG_SSH;
            core->registers[DSP_REG_SR] = sc << DSP_SR_SC;
            core->registers[DSP_REG_SP] = low ? 1 : 0;
            core->pc = 0x100;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f400 | reg);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0xabcdef);
            dsp56k_execute_instruction(core);
            CHECK(core->stack[low][1] == (sc ? 0xcdefu : 0xabcdefu));
            core->registers[reg] = core->stack[low][1] = 0xabcdef;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x044480 | reg);
            dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_X0] == (sc ? 0xcdefu : 0xabcdefu));
            CHECK(core->registers[DSP_REG_SP] == (low ? 1u : 0u));
        }
    }
    dsp56k_reset_cpu(core);
    core->registers[DSP_REG_SR] = 0x102000; /* Compatibility plus full-width status. */
    core->pc = 0x100;
    dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0d0200); /* Implicit JSR push. */
    dsp56k_execute_instruction(core);
    CHECK(core->stack[0][1] == 0x101 && core->stack[1][1] == 0x102000);
    dsp56k_reset_cpu(core);
}

static void check_stack_memory_moves(dsp_core_t *core)
{
    for (unsigned sc = 0; sc < 2; sc++)
    for (unsigned low = 0; low < 2; low++)
    for (unsigned space = 0; space < 2; space++)
    for (unsigned ea = 0; ea < 2; ea++)
    for (unsigned load = 0; load < 2; load++) {
        dsp56k_reset_cpu(core);
        unsigned reg = low ? DSP_REG_SSL : DSP_REG_SSH;
        unsigned address = ea ? 0x120 : 0x10;
        core->registers[DSP_REG_SR] = sc << DSP_SR_SC;
        core->registers[DSP_REG_SP] = load && !low ? 0 : 1;
        core->registers[reg] = core->stack[low][1] = 0xabcdef;
        dsp56k_write_memory(core, space, address, load ? 0xabcdef : 0x555555);
        core->pc = 0x100;
        uint32_t opcode = (ea ? 0x057000 : 0x051000) |
                          (load << 15) | (space << 6) | reg;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, opcode);
        if (ea) dsp56k_write_memory(core, DSP_SPACE_P, 0x101, address);
        dsp56k_execute_instruction(core);
        uint32_t expected = sc ? 0xcdef : 0xabcdef;
        if (load) CHECK(core->stack[low][1] == expected);
        else CHECK(dsp56k_read_memory(core, space, address) == expected);
        CHECK(core->registers[DSP_REG_SP] == (low || load ? 1u : 0u));
        CHECK(core->pc == 0x101 + ea && core->interrupt_pipeline_count == 0);
    }
    dsp56k_reset_cpu(core);
}

static void check_extended_stack(dsp_core_t *core)
{
    for (unsigned space = 0; space < 2; space++)
    for (unsigned initial = 0; initial < 16; initial++) {
        dsp56k_reset_cpu(core);
        core->registers[DSP_REG_OMR] = (1u << DSP_OMR_SEN) | (space << DSP_OMR_XYS);
        unsigned depth = 16 + initial;
        unsigned total = depth + 32;
        core->registers[DSP_REG_SP] = depth;
        core->registers[DSP_REG_SC] = 14;
        core->registers[DSP_REG_EP] = 0x200 + (depth-14)*2;
        for (unsigned i = 1; i <= depth; i++) {
            if (i <= depth-14) {
                dsp56k_write_memory(core, space, 0x200+(i-1)*2, 0x200000+i);
                dsp56k_write_memory(core, space, 0x201+(i-1)*2, 0x100000+i);
            } else {
                core->stack[0][i&15] = 0x100000+i;
                core->stack[1][i&15] = 0x200000+i;
            }
        }
        core->registers[DSP_REG_SSH] = core->stack[0][depth&15];
        core->registers[DSP_REG_SSL] = core->stack[1][depth&15];
        core->registers[DSP_REG_SZ] = 64;
        for (unsigned i = depth+1; i <= total; i++) {
            core->pc = 0x100;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f43c);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x100000 + i);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x05f43d);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x103, 0x200000 + i);
            dsp56k_execute_instruction(core);
            dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_SP] == i);
            CHECK(core->registers[DSP_REG_SC] == (i > 14 ? 14u : i));
        }
        CHECK(core->registers[DSP_REG_EP] == 0x200+(total-14)*2);
        for (unsigned i = 1; i <= total-14; i++) {
            CHECK(dsp56k_read_memory(core, space, 0x200 + (i-1)*2) == 0x200000 + i);
            CHECK(dsp56k_read_memory(core, space, 0x201 + (i-1)*2) == 0x100000 + i);
        }
        for (unsigned i = total; i; i--) {
            core->pc = 0x100;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0444bd);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x0445bc);
            dsp56k_execute_instruction(core);
            dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_X0] == 0x200000 + i);
            CHECK(core->registers[DSP_REG_X1] == 0x100000 + i);
            CHECK(core->registers[DSP_REG_SP] == i-1);
        }
        CHECK(core->registers[DSP_REG_EP] == 0x200);
        CHECK(core->registers[DSP_REG_SC] == 0);
        CHECK(core->interrupt_pipeline_count == 0);
    }
    core->registers[DSP_REG_SZ] = core->registers[DSP_REG_EP] = 0;
    dsp56k_reset_cpu(core);
}

static void check_extended_stack_faults(dsp_core_t *core)
{
    for (unsigned space = 0; space < 2; space++)
    for (unsigned pop = 0; pop < 2; pop++)
    for (unsigned sticky = 0; sticky < 2; sticky++) {
        dsp56k_reset_cpu(core);
        unsigned flag = pop ? DSP_OMR_EUN : DSP_OMR_EOV;
        uint32_t omr = (1u << DSP_OMR_SEN) | (space << DSP_OMR_XYS) |
                       (sticky << flag);
        core->registers[DSP_REG_OMR] = omr;
        core->registers[DSP_REG_SR] = 0x300;
        core->registers[DSP_REG_SP] = pop ? 0 : 1;
        core->registers[DSP_REG_SC] = pop ? 0 : 1;
        core->registers[DSP_REG_SZ] = 1;
        core->registers[DSP_REG_EP] = 0x200;
        core->registers[DSP_REG_SSH] = 0xabcdef;
        dsp56k_write_memory(core, space, 0x200, 0x123456);
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, pop ? 0x0444bc : 0x05f43c);
        if (!pop) dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0xabcdef);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_OMR] == (omr | (1u << flag)));
        CHECK(core->registers[DSP_REG_SP] == (pop ? 0xffffffu : 2u));
        CHECK((core->interrupt_pipeline_count != 0) == !sticky);
        CHECK(core->registers[DSP_REG_EP] == 0x200);
        CHECK(dsp56k_read_memory(core, space, 0x200) == 0x123456);
        if (pop) CHECK(core->registers[DSP_REG_X0] == 0xabcdef);
        /* Guest OMR MOVE clears the sticky flag; no CPU reset. */
        core->pc = 0x110;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x110, 0x05f43a);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x111, omr & ~(1u << flag));
        dsp56k_execute_instruction(core);
        CHECK(!(core->registers[DSP_REG_OMR] & (1u << flag)));
    }
    core->registers[DSP_REG_EP] = core->registers[DSP_REG_SZ] = 0;
    dsp56k_reset_cpu(core);
}

static void check_extended_fault_rearm(dsp_core_t *core)
{
    for (unsigned pop = 0; pop < 2; pop++) {
        dsp56k_reset_cpu(core);
        core->registers[DSP_REG_SR] = 0x300;
        core->registers[DSP_REG_OMR] = 1u << DSP_OMR_SEN;
        core->registers[DSP_REG_SZ] = 1;
        dsp56k_write_memory(core, DSP_SPACE_P, 2, 0x014180); /* Fast handler ADD. */
        dsp56k_write_memory(core, DSP_SPACE_P, 3, 0);
        for (unsigned attempt = 1; attempt <= 2; attempt++) {
            const uint32_t setup[] = {
                0x05f43a, 1u << DSP_OMR_SEN,
                0x05f43b, pop ? 0 : 1,
                0x05f431, pop ? 0 : 1
            };
            core->pc = 0x100;
            for (unsigned i = 0; i < 6; i++)
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100+i, setup[i]);
            for (unsigned i = 0; i < 3; i++) dsp56k_execute_instruction(core);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x106, pop ? 0x0444bc : 0x05f43c);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x107, pop ? 0 : 0xabcdef);
            for (unsigned i = 0x108; i < 0x120; i++)
                dsp56k_write_memory(core, DSP_SPACE_P, i, 0);
            for (unsigned i = 0; i < 12; i++) dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_A1] == attempt);
            CHECK(core->registers[DSP_REG_OMR] & (1u << (pop ? DSP_OMR_EUN : DSP_OMR_EOV)));
            CHECK(core->interrupt_counter == 0 && core->interrupt_pipeline_count == 0);
        }
    }
    core->registers[DSP_REG_EP] = core->registers[DSP_REG_SZ] = 0;
    dsp56k_reset_cpu(core);
}

static void check_extended_calls(dsp_core_t *core)
{
    for (unsigned space = 0; space < 2; space++)
    for (unsigned rti = 0; rti < 2; rti++)
    for (unsigned compatibility = 0; compatibility < 2; compatibility++) {
        dsp56k_reset_cpu(core);
        /* Initialize extension through uploaded guest MOVEC instructions. */
        const uint32_t setup[] = {0x05f42a, 0x200, 0x05f438, 64,
            0x05f43a, (1u << DSP_OMR_SEN) | (space << DSP_OMR_XYS)};
        for (unsigned i = 0; i < 6; i++)
            dsp56k_write_memory(core, DSP_SPACE_P, 0x80+i, setup[i]);
        core->pc = 0x80;
        for (unsigned i = 0; i < 3; i++) dsp56k_execute_instruction(core);
        core->pc = 0x100;
        for (unsigned i = 0; i < 32; i++) {
            unsigned addr = 0x100 + i*4;
            dsp56k_write_memory(core, DSP_SPACE_P, addr, 0x0d0000 | (addr+4));
            dsp56k_write_memory(core, DSP_SPACE_P, addr+1, i ? (rti ? 0x000004 : 0x00000c) : 0x014180);
        }
        dsp56k_write_memory(core, DSP_SPACE_P, 0x180, rti ? 0x000004 : 0x00000c); /* Deepest return. */
        for (unsigned i = 0; i < 32; i++) {
            core->registers[DSP_REG_SR] = 0x100000 | (compatibility << DSP_SR_SC) | (i & 7);
            dsp56k_execute_instruction(core);
            CHECK(core->pc == 0x104+i*4 && core->registers[DSP_REG_SP] == i+1);
        }
        CHECK(core->registers[DSP_REG_SC] == 14 && core->registers[DSP_REG_EP] == 0x224);
        for (unsigned i = 0; i < 18; i++) {
            CHECK(dsp56k_read_memory(core, space, 0x200+i*2) == (0x100000u | (compatibility << DSP_SR_SC) | (i & 7)));
            CHECK(dsp56k_read_memory(core, space, 0x201+i*2) == 0x101+i*4);
        }
        for (unsigned i = 32; i; i--) {
            dsp56k_execute_instruction(core);
            CHECK(core->pc == 0x101+(i-1)*4 && core->registers[DSP_REG_SP] == i-1);
            CHECK(core->registers[DSP_REG_SR] == (0x100000u | (compatibility << DSP_SR_SC) | (rti ? (i-1)&7 : 7)));
        }
        CHECK(core->registers[DSP_REG_SC] == 0 && core->registers[DSP_REG_EP] == 0x200);
        CHECK(core->interrupt_pipeline_count == 0);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_A1] == 1 && core->pc == 0x102);
    }
    core->registers[DSP_REG_EP] = core->registers[DSP_REG_SZ] = 0;
    dsp56k_reset_cpu(core);
}

static void check_extended_loop_stack(dsp_core_t *core)
{
    for (unsigned space = 0; space < 2; space++)
    for (unsigned automatic = 0; automatic < 2; automatic++) {
        dsp56k_reset_cpu(core);
        core->registers[DSP_REG_OMR] = (1u << DSP_OMR_SEN) | (space << DSP_OMR_XYS);
        core->registers[DSP_REG_EP] = 0x200;
        core->registers[DSP_REG_SZ] = 64;
        core->registers[DSP_REG_LA] = 0x55;
        core->registers[DSP_REG_LC] = 0xabcdef;
        core->registers[DSP_REG_SR] = 0x100000;
        for (unsigned i = 0; i < 20; i++) {
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100+i*2, 0x060180);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x101+i*2, automatic ? 0x214-i : 0x300);
            dsp56k_write_memory(core, DSP_SPACE_P, automatic ? 0x201+i : 0x128+i, automatic ? 0x014180 : 0x00008c);
        }
        core->pc = 0x100;
        for (unsigned i = 0; i < 20; i++) {
            dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_SP] == (i+1)*2);
            CHECK(core->registers[DSP_REG_LA] == (automatic ? 0x214-i : 0x300) && core->registers[DSP_REG_LC] == 1);
        }
        CHECK(core->registers[DSP_REG_SC] == 14 && core->registers[DSP_REG_EP] == 0x234);
        CHECK(dsp56k_read_memory(core, space, 0x200) == 0xabcdef);
        CHECK(dsp56k_read_memory(core, space, 0x201) == 0x55);
        CHECK(dsp56k_read_memory(core, space, 0x202) == 0x100000);
        CHECK(dsp56k_read_memory(core, space, 0x203) == 0x102);
        if (automatic) {
            dsp56k_write_memory(core, DSP_SPACE_P, 0x128, 0x0c0201); /* JMP to inner endpoint. */
            dsp56k_execute_instruction(core);
            CHECK(core->pc == 0x201);
        }
        for (unsigned i = 20; i; i--) {
            dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_SP] == (i-1)*2);
            CHECK(core->registers[DSP_REG_LA] == (i == 1 ? 0x55u : automatic ? 0x216u-i : 0x300u));
            CHECK(core->registers[DSP_REG_LC] == (i == 1 ? 0xabcdefu : 1u));
            CHECK((core->registers[DSP_REG_SR] & (automatic ? 0xffff00u : 0xffffffu)) ==
                  (0x100000u | (i > 1 ? 1u << DSP_SR_LF : 0)));
        }
        CHECK(core->registers[DSP_REG_SC] == 0 && core->registers[DSP_REG_EP] == 0x200);
        CHECK(core->pc == (automatic ? 0x215u : 0x13cu) && core->interrupt_pipeline_count == 0);
        if (automatic) CHECK(core->registers[DSP_REG_A1] == 20);
    }
    core->registers[DSP_REG_EP] = core->registers[DSP_REG_SZ] = 0;
    dsp56k_reset_cpu(core);
}

static void check_extended_sp_move(dsp_core_t *core)
{
    for (unsigned sc = 0; sc < 2; sc++) {
        dsp56k_reset_cpu(core);
        core->registers[DSP_REG_OMR] = 1u << DSP_OMR_SEN;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f43b);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0xabcdef);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_SP] == 0xabcdef && core->interrupt_pipeline_count == 0);
        core->registers[DSP_REG_SR] = sc << DSP_SR_SC;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x0444bb); /* SP,X0 */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x103, 0x05103b); /* SP,x:$10 */
        dsp56k_execute_instruction(core);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_X0] == (sc ? 0xcdefu : 0xabcdefu));
        CHECK(dsp56k_read_memory(core, DSP_SPACE_X, 0x10) == (sc ? 0xcdefu : 0xabcdefu));
        CHECK(core->registers[DSP_REG_SP] == 0xabcdef && core->interrupt_pipeline_count == 0);
        const uint32_t values[] = {0, 0xabcdef, 0xffffff, 0x123450};
        for (unsigned form = 0; form < 3; form++)
        for (unsigned i = 0; i < 4; i++) {
            uint32_t expected = values[i] & (sc ? 0xffff : 0xffffff);
            unsigned top = expected & 15;
            core->stack[0][top] = 0x100000 + top;
            core->stack[1][top] = 0x200000 + top;
            core->registers[DSP_REG_X0] = values[i];
            dsp56k_write_memory(core, DSP_SPACE_X, 0x10, values[i]);
            core->pc = 0x110;
            uint32_t opcode = form == 0 ? 0x05f43b : form == 1 ? 0x04c4bb : 0x05903b;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x110, opcode);
            if (!form) dsp56k_write_memory(core, DSP_SPACE_P, 0x111, values[i]);
            dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_SP] == expected);
            CHECK(core->registers[DSP_REG_SSH] == 0x100000 + top);
            CHECK(core->registers[DSP_REG_SSL] == 0x200000 + top);
            CHECK(core->interrupt_pipeline_count == 0);
            CHECK(core->pc == (form ? 0x111u : 0x112u));
        }
    }
    dsp56k_reset_cpu(core);
}

static void check_normal_stack_count(dsp_core_t *core)
{
    dsp56k_reset_cpu(core);
    for (unsigned i = 1; i <= 15; i++) {
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f43c);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x100000+i);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_SC] == i);
    }
    for (unsigned i = 15; i; i--) {
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0445bc);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_SC] == i-1);
        CHECK(core->registers[DSP_REG_X1] == 0x100000+i);
    }
    dsp56k_reset_cpu(core);
}

static void check_stack_enable(dsp_core_t *core)
{
    for (unsigned space = 0; space < 2; space++) {
        dsp56k_reset_cpu(core);
        core->registers[DSP_REG_EP] = 0x200;
        core->registers[DSP_REG_SZ] = 64;
        for (unsigned i = 1; i <= 32; i++) {
            if (i == 15) {
                core->pc = 0x110;
                dsp56k_write_memory(core, DSP_SPACE_P, 0x110, 0x05f43a);
                dsp56k_write_memory(core, DSP_SPACE_P, 0x111,
                    (1u << DSP_OMR_SEN) | (space << DSP_OMR_XYS));
                dsp56k_execute_instruction(core);
                CHECK(core->registers[DSP_REG_SP] == 14);
                CHECK(core->registers[DSP_REG_SC] == 14);
            }
            core->pc = 0x100;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f43c);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x100000+i);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x05f43d);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x103, 0x200000+i);
            dsp56k_execute_instruction(core);
            dsp56k_execute_instruction(core);
        }
        CHECK(core->registers[DSP_REG_SP] == 32);
        CHECK(core->registers[DSP_REG_SC] == 14);
        CHECK(core->registers[DSP_REG_EP] == 0x224);
        for (unsigned i = 1; i <= 18; i++) {
            CHECK(dsp56k_read_memory(core, space, 0x200+2*(i-1)) == 0x200000+i);
            CHECK(dsp56k_read_memory(core, space, 0x201+2*(i-1)) == 0x100000+i);
        }
        for (unsigned i = 32; i; i--) {
            core->pc = 0x100;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0444bd);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x0445bc);
            dsp56k_execute_instruction(core);
            dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_X0] == 0x200000+i);
            CHECK(core->registers[DSP_REG_X1] == 0x100000+i);
        }
        CHECK(core->registers[DSP_REG_SP] == 0 && core->registers[DSP_REG_SC] == 0);
        CHECK(core->registers[DSP_REG_EP] == 0x200);
    }
    core->registers[DSP_REG_EP] = core->registers[DSP_REG_SZ] = 0;
    dsp56k_reset_cpu(core);
}

static void check_extended_sc_refill(dsp_core_t *core)
{
    for (unsigned space = 0; space < 2; space++) {
        for (unsigned depth = 0; depth <= 3; depth++) {
            for (unsigned count = 0; count <= 1 && count <= depth; count++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_OMR] = (1u << DSP_OMR_SEN) | (space << DSP_OMR_XYS);
                core->registers[DSP_REG_SP] = depth;
                core->registers[DSP_REG_EP] = 0x200 + 2*(depth-count);
                for (unsigned i = 1; i <= depth; i++) {
                    if (i <= depth-count) {
                        dsp56k_write_memory(core, space, 0x200+2*(i-1), 0x200000+i);
                        dsp56k_write_memory(core, space, 0x201+2*(i-1), 0x100000+i);
                    } else {
                        core->stack[0][i] = 0x100000+i;
                        core->stack[1][i] = 0x200000+i;
                    }
                }
                core->pc = 0x100;
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f431);
                dsp56k_write_memory(core, DSP_SPACE_P, 0x101, count);
                dsp56k_execute_instruction(core);
                unsigned filled = depth < 2 ? depth : 2;
                CHECK(core->registers[DSP_REG_SC] == filled);
                CHECK(core->registers[DSP_REG_SP] == depth);
                CHECK(core->registers[DSP_REG_EP] == 0x200+2*(depth-filled));
                CHECK(!(core->registers[DSP_REG_OMR] & (1u << DSP_OMR_WRP)));
                for (unsigned i = depth-filled+1; i <= depth; i++) {
                    CHECK(core->stack[0][i] == 0x100000+i);
                    CHECK(core->stack[1][i] == 0x200000+i);
                }
                if (depth) {
                    CHECK(core->registers[DSP_REG_SSH] == 0x100000+depth);
                    CHECK(core->registers[DSP_REG_SSL] == 0x200000+depth);
                }
            }
        }
    }
    core->registers[DSP_REG_EP] = core->registers[DSP_REG_SZ] = 0;
    dsp56k_reset_cpu(core);
}

static void check_extended_double_spill(dsp_core_t *core)
{
    for (unsigned space = 0; space < 2; space++) {
        dsp56k_reset_cpu(core);
        core->registers[DSP_REG_OMR] = (1u << DSP_OMR_SEN) | (space << DSP_OMR_XYS);
        core->registers[DSP_REG_EP] = 0x200;
        core->registers[DSP_REG_SZ] = 64;
        core->registers[DSP_REG_SP] = 15;
        for (unsigned i = 1; i <= 15; i++) {
            core->stack[0][i] = 0x100000+i;
            core->stack[1][i] = 0x200000+i;
        }
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f431); /* Restore SC=15. */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 15);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x05f43c);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x103, 0x100010);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x104, 0x05f43d);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x105, 0x200010);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_SC] == 14 && core->registers[DSP_REG_EP] == 0x202);
        CHECK(dsp56k_read_memory(core, space, 0x200) == 0x200001);
        CHECK(dsp56k_read_memory(core, space, 0x201) == 0x100001);
        for (unsigned i = 0; i < 2; i++) dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_SC] == 14 && core->registers[DSP_REG_EP] == 0x204);
        for (unsigned i = 1; i <= 2; i++) {
            CHECK(dsp56k_read_memory(core, space, 0x200+(i-1)*2) == 0x200000+i);
            CHECK(dsp56k_read_memory(core, space, 0x201+(i-1)*2) == 0x100000+i);
        }
        for (unsigned i = 16; i; i--) {
            core->pc = 0x110;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x110, 0x0444bd);
            dsp56k_write_memory(core, DSP_SPACE_P, 0x111, 0x0445bc);
            dsp56k_execute_instruction(core);
            dsp56k_execute_instruction(core);
            CHECK(core->registers[DSP_REG_X0] == 0x200000+i);
            CHECK(core->registers[DSP_REG_X1] == 0x100000+i);
        }
        CHECK(core->registers[DSP_REG_EP] == 0x200 && core->registers[DSP_REG_SC] == 0);
    }
    core->registers[DSP_REG_EP] = core->registers[DSP_REG_SZ] = 0;
    dsp56k_reset_cpu(core);
}

static void check_stack_error_delivery(dsp_core_t *core)
{
    for (unsigned fault = 0; fault < 3; fault++) {
        dsp56k_reset_cpu(core);
        core->registers[DSP_REG_SR] = 0x300; /* Stack error survives masked IPL. */
        core->pc = 0x100;
        for (unsigned pc = 0x101; pc < 0x110; pc++) dsp56k_write_memory(core, DSP_SPACE_P, pc, 0);
        dsp56k_write_memory(core, DSP_SPACE_P, 2, 0x0d0200); /* Stack-error vector JSR. */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x200, 0x05f43b); /* Handler repairs SP. */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x201, 0);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x202, 0x014180); /* Actual handler ADD. */
        if (fault == 0) {
            core->registers[DSP_REG_SP] = 15;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f43c); /* MOVEC #value,SSH overflow. */
            dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x123);
        } else if (fault == 1) {
            core->registers[DSP_REG_SSH] = 0x101;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0xc); /* RTS on empty stack. */
        } else {
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x05f43b); /* Explicit SP error bit. */
            dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 1u << DSP_SP_SE);
        }
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_SP] & (1u << DSP_SP_SE));
        for (unsigned step = 0; core->pc != 0x200 && step < 12; step++)
            dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x200);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_SP] == 0 && core->pc == 0x202);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_A1] == 1);
        /* Continue guest flow: clearing SE must permit a fresh stack request. */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x203, 0x05f43b);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x204, 1u << DSP_SP_SE);
        for (unsigned pc = 0x205; pc < 0x210; pc++) dsp56k_write_memory(core, DSP_SPACE_P, pc, 0);
        CHECK(core->interrupt_counter == 0 && !core->interrupt_is_pending[DSP_INTER_STACK_ERROR]);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_SP] & (1u << DSP_SP_SE));
        for (unsigned step = 0; core->pc != 0x200 && step < 12; step++)
            dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x200);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_SP] == 0);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_A1] == 2 && core->interrupt_counter == 0);
    }
    dsp56k_reset_cpu(core);
}

static void check_trap_delivery(dsp_core_t *core)
{
    for (unsigned kind = 0; kind < 3; kind++)
    for (unsigned zero = 0; zero < 2; zero++) {
        dsp56k_reset_cpu(core);
        uint32_t status = 0xc00080 | (zero << DSP_SR_Z);
        core->registers[DSP_REG_SR] = status;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, kind == 0 ? 6 : (kind == 1 ? 0x1a : 0x12));
        for (unsigned address = 0x101; address < 0x110; address++)
            dsp56k_write_memory(core, DSP_SPACE_P, address, 0);
        dsp56k_write_memory(core, DSP_SPACE_P, 8, 0x0d0200); /* vector JSR $200 */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x200, 4); /* RTI */
        dsp56k_execute_instruction(core);
        bool taken = kind == 0 || (kind == 1 ? zero : !zero);
        if (!taken) {
            CHECK(core->pc == 0x101 && core->interrupt_counter == 0);
            CHECK(core->registers[DSP_REG_SP] == 0);
            continue;
        }
        for (unsigned step = 0; core->pc != 0x200 && step < 8; step++)
            dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x200 && core->registers[DSP_REG_SP] == 1);
        CHECK(core->registers[DSP_REG_SR] == (status | 0x300));
        uint32_t resume = core->registers[DSP_REG_SSH];
        CHECK(core->registers[DSP_REG_SSL] == status);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == resume && core->registers[DSP_REG_SR] == status);
        CHECK(core->registers[DSP_REG_SP] == 0);
    }
    dsp56k_reset_cpu(core);
}

static void check_repeat_fetch_and_interrupt(dsp_core_t *core)
{
    for (unsigned compatibility = 0; compatibility < 2; compatibility++) {
        dsp56k_reset_cpu(core);
        core->registers[DSP_REG_SR] = compatibility << DSP_SR_SC;
        core->registers[DSP_REG_X0] = 3;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x06c420); /* REP X0 */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x014180); /* ADD #1,A */
        for (unsigned address = 0x102; address < 0x110; address++)
            dsp56k_write_memory(core, DSP_SPACE_P, address, 0);
        dsp56k_write_memory(core, DSP_SPACE_P, 8, 0x0d0200);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x200, 4);
        dsp56k_execute_instruction(core);
        dsp56k_execute_instruction(core);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x014184); /* SUB #1,A */
        dsp56k_add_interrupt(core, DSP_INTER_TRAP);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x101 && core->registers[DSP_REG_A1] == 2);
        CHECK(core->interrupt_counter == 1 && core->registers[DSP_REG_SP] == 0);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x102 && core->registers[DSP_REG_A1] == 3 && !core->loop_rep);
        for (unsigned i = 0; core->pc != 0x200 && i < 8; i++) dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x200 && core->registers[DSP_REG_SP] == 1);
        dsp56k_execute_instruction(core);
        /* Finish the existing interrupt pipeline before returning to the rewritten word. */
        for (unsigned i = 0; i < 4; i++) dsp56k_execute_instruction(core);
        core->pc = 0x101;
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_A1] == 2);
    }
    dsp56k_reset_cpu(core);
}

static void check_immediate_accumulate(dsp_core_t *core)
{
    const uint32_t operands[] = {0, 1, 0x7fffff, 0x800000, 0xffffff};
    const uint64_t accumulators[] = {0, 1, 0x7fffffffffffffULL, 0x80000000000000ULL, 0xffffffffffffffULL};
    const unsigned regs[] = {DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1, DSP_REG_Y1};
    const unsigned power_regs[] = {DSP_REG_Y1, DSP_REG_X0, DSP_REG_Y0, DSP_REG_X1};
    for (unsigned power = 0; power < 2; power++)
    for (unsigned rounded = 0; rounded < 2; rounded++)
    for (unsigned accumulate = 0; accumulate < 2; accumulate++)
    for (unsigned rounding_mode = 0; rounding_mode < (rounded ? 2u : 1u); rounding_mode++)
    for (unsigned scale = 0; scale < (rounded ? 3u : 1u); scale++)
    for (unsigned q = 0; q < 4; q++)
    for (unsigned d = 0; d < 2; d++)
    for (unsigned negate = 0; negate < 2; negate++)
    for (unsigned x = 0; x < (power ? 22u : 5u); x++)
    for (unsigned y = 0; y < 5; y++)
    for (unsigned a = 0; a < 5; a++) {
        uint32_t coefficient = power ? 1u << (22 - x) : operands[x];
        int64_t left = (int32_t)(coefficient << 8) / 256;
        int64_t right = (int32_t)(operands[y] << 8) / 256;
        int64_t product = left * right * (negate ? -2 : 2);
        uint64_t initial = accumulate ? accumulators[a] : 0;
        uint64_t expected = (initial + (uint64_t)product) & 0xffffffffffffffULL;
        if (rounded) {
            unsigned shift = scale == 1 ? 25 : scale == 2 ? 23 : 24;
            uint64_t unit = 1ULL << shift, half = unit / 2;
            uint64_t quotient = expected / unit, remainder = expected % unit;
            if (remainder > half || (remainder == half && (rounding_mode || (quotient & 1)))) quotient++;
            expected = (quotient * unit) & 0xffffffffffffffULL;
        }
        unsigned high = d ? DSP_REG_B2 : DSP_REG_A2;
        unsigned middle = d ? DSP_REG_B1 : DSP_REG_A1;
        unsigned low = d ? DSP_REG_B0 : DSP_REG_A0;
        core->registers[DSP_REG_SR] = 0xc00001 | (rounding_mode << 21) | (scale == 1 ? 1u << DSP_SR_S0 : scale == 2 ? 1u << DSP_SR_S1 : 0);
        core->registers[power ? power_regs[q] : regs[q]] = operands[y];
        core->registers[high] = (uint32_t)(accumulators[a] >> 48);
        core->registers[middle] = (uint32_t)(accumulators[a] >> 24) & 0xffffff;
        core->registers[low] = (uint32_t)accumulators[a] & 0xffffff;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, (power ? 0x0100c0 | ((x + 1) << 8) : 0x0141c0) | rounded | (accumulate << 1) | (q << 4) | (d << 3) | (negate << 2));
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, coefficient);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == (power ? 0x101u : 0x102u));
        CHECK(core->registers[high] == (expected >> 48));
        CHECK(core->registers[middle] == ((expected >> 24) & 0xffffff));
        CHECK(core->registers[low] == (expected & 0xffffff));
        CHECK((core->registers[DSP_REG_SR] & 0xff0001) == (0xc00001 | (rounding_mode << 21)));
        uint64_t unrounded = (initial + (uint64_t)product) & 0xffffffffffffffULL;
        bool overflow = accumulate && ((initial >> 55) == (((uint64_t)product >> 55) & 1)) && ((initial >> 55) != (unrounded >> 55));
        CHECK(!!(core->registers[DSP_REG_SR] & (1u << DSP_SR_V)) == overflow);
    }
}

static void check_rounding_modes(dsp_core_t *core)
{
    const uint64_t upper[] = {0, 1, 2, 0x7fffff, 0x800000, 0xffffffff};
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
    for (unsigned scale = 0; scale < 3; scale++)
    for (unsigned mode = 0; mode < 2; mode++)
    for (unsigned d = 0; d < 2; d++)
    for (unsigned u = 0; u < 6; u++)
    for (unsigned fraction = 0; fraction < 5; fraction++) {
        unsigned width = sixteen ? 16 : 24;
        unsigned shift = width + (scale == 1 ? 1 : 0) - (scale == 2 ? 1 : 0);
        uint64_t mask = (1ULL << (2*width+8)) - 1;
        uint64_t half = 1ULL << (shift - 1), unit = half * 2;
        uint64_t low[] = {0, half - 1, half, half + 1, unit - 1};
        uint64_t input = ((upper[u] << shift) | low[fraction]) & mask;
        uint64_t quotient = input / unit, remainder = input % unit;
        if (remainder > half || (remainder == half && (mode || (quotient & 1)))) quotient++;
        uint64_t expected = (quotient * unit) & mask;
        unsigned high = d ? DSP_REG_B2 : DSP_REG_A2;
        unsigned middle = d ? DSP_REG_B1 : DSP_REG_A1;
        unsigned bottom = d ? DSP_REG_B0 : DSP_REG_A0;
        core->registers[DSP_REG_SR] = 0xc00001 | (sixteen << DSP_SR_SA) | (mode << 21) | (scale == 1 ? 1u << DSP_SR_S0 : scale == 2 ? 1u << DSP_SR_S1 : 0);
        core->registers[high] = (uint32_t)(input >> (2*width));
        core->registers[middle] = (uint32_t)((input >> width) & ((1u << width)-1)) << (sixteen ? 8 : 0);
        core->registers[bottom] = ((uint32_t)input & ((1u << width)-1)) << (sixteen ? 8 : 0);
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x200011 | (d << 3)); /* RND */
        dsp56k_execute_instruction(core);
        CHECK(core->registers[high] == (expected >> (2*width)));
        CHECK(core->registers[middle] == (((expected >> width) & ((1u << width)-1)) << (sixteen ? 8 : 0)));
        CHECK(core->registers[bottom] == ((expected & ((1u << width)-1)) << (sixteen ? 8 : 0)));
        CHECK((core->registers[DSP_REG_SR] & 0xe00001) == (0xc00001 | (mode << 21)));
    }
}

static void check_mixed_multiply(dsp_core_t *core)
{
    const unsigned pairs[16][2] = {{0,0},{2,2},{1,0},{3,2},{0,3},{2,0},{1,2},{3,1},
                                 {1,1},{3,3},{0,1},{2,3},{3,0},{0,2},{2,1},{1,3}};
    const unsigned regs[] = {DSP_REG_X0,DSP_REG_X1,DSP_REG_Y0,DSP_REG_Y1};
    const uint32_t values[] = {0,1,0x7fffff,0x800000,0xffffff};
    const uint64_t initial[] = {0,0x7fffffffffffffULL,0x80000000000000ULL};
    for (unsigned double_precision = 0; double_precision < 2; double_precision++)
    for (unsigned accumulate = 0; accumulate < 2; accumulate++)
    for (unsigned unsign = 0; unsign < (double_precision ? 3u : 2u); unsign++)
    for (unsigned negate = 0; negate < 2; negate++)
    for (unsigned d = 0; d < 2; d++)
    for (unsigned pair = 0; pair < 16; pair++)
    for (unsigned x = 0; x < 5; x++)
    for (unsigned y = 0; y < 5; y++)
    for (unsigned a = 0; a < 3; a++) {
        if (double_precision && !accumulate) continue;
        unsigned mode = double_precision ? (unsign == 0 ? 0 : unsign == 1 ? 2 : 3) : unsign ? 3 : 2;
        core->registers[regs[pairs[pair][0]]] = values[x];
        core->registers[regs[pairs[pair][1]]] = values[y];
        int64_t left = core->registers[regs[pairs[pair][0]]];
        int64_t right = core->registers[regs[pairs[pair][1]]];
        if (mode != 3 && left >= 0x800000) left -= 0x1000000;
        if (mode == 0 && right >= 0x800000) right -= 0x1000000;
        int64_t product = left * right * (negate ? -2 : 2);
        int64_t signed_initial = (int64_t)initial[a];
        if (initial[a] & (1ULL << 55)) signed_initial -= (1LL << 56);
        int64_t shifted = signed_initial / 0x1000000;
        if (signed_initial < 0 && signed_initial % 0x1000000) shifted--;
        uint64_t expected = ((double_precision ? (uint64_t)shifted : accumulate ? initial[a] : 0) + (uint64_t)product) & 0xffffffffffffffULL;
        unsigned high = d ? DSP_REG_B2 : DSP_REG_A2;
        unsigned middle = d ? DSP_REG_B1 : DSP_REG_A1;
        unsigned low = d ? DSP_REG_B0 : DSP_REG_A0;
        core->registers[high] = (uint32_t)(initial[a] >> 48);
        core->registers[middle] = (uint32_t)(initial[a] >> 24) & 0xffffff;
        core->registers[low] = (uint32_t)initial[a] & 0xffffff;
        core->registers[DSP_REG_SR] = 0xc00001;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
            (double_precision ? 0x012480 | ((mode >> 1) << 8) | ((mode & 1) << 6) : (accumulate ? 0x012680 : 0x012780) | (unsign << 6)) | (d << 5) | (negate << 4) | pair);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x101);
        CHECK(core->registers[high] == (expected >> 48));
        CHECK(core->registers[middle] == ((expected >> 24) & 0xffffff));
        CHECK(core->registers[low] == (expected & 0xffffff));
        CHECK((core->registers[DSP_REG_SR] & 0xff0001) == 0xc00001);
    }
}

static void check_bit_field_extract(dsp_core_t *core)
{
    const unsigned controls[] = {0,DSP_REG_A1,DSP_REG_B1,DSP_REG_X0,DSP_REG_Y0,DSP_REG_X1,DSP_REG_Y1};
    const uint64_t patterns[] = {0,0xffffffffffffffULL,0x80000000000000ULL,0x0123456789abcdULL,0xaaaa5555aaaa55ULL};
    for (unsigned unsign = 0; unsign < 2; unsign++)
    for (unsigned src = 0; src < 2; src++)
    for (unsigned dst = 0; dst < 2; dst++)
    for (unsigned form = 0; form < 7; form++)
    for (unsigned width = 1; width <= 56; width++)
    for (unsigned offset = 0; offset + width <= 56; offset++)
    for (unsigned pattern = 0; pattern < 5; pattern++) {
        uint32_t control = (width << 12) | offset;
        unsigned sh = src ? DSP_REG_B2 : DSP_REG_A2;
        unsigned sm = src ? DSP_REG_B1 : DSP_REG_A1;
        unsigned sl = src ? DSP_REG_B0 : DSP_REG_A0;
        core->registers[sh] = (uint32_t)(patterns[pattern] >> 48);
        core->registers[sm] = (uint32_t)(patterns[pattern] >> 24) & 0xffffff;
        core->registers[sl] = (uint32_t)patterns[pattern] & 0xffffff;
        if (form) core->registers[controls[form]] = control;
        uint64_t input = ((uint64_t)core->registers[sh] << 48) | ((uint64_t)core->registers[sm] << 24) | core->registers[sl];
        uint64_t expected = 0;
        for (unsigned bit = 0; bit < width; bit++)
            expected |= ((input >> (offset + bit)) & 1) << bit;
        if (!unsign && ((input >> (offset + width - 1)) & 1))
            for (unsigned bit = width; bit < 56; bit++) expected |= 1ULL << bit;
        core->registers[DSP_REG_SR] = 0xc000c3;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
            (form ? 0x0c1a00 | ((form + 1) << 1) : 0x0c1800) | (unsign << 7) | (src << 4) | dst);
        if (!form) dsp56k_write_memory(core, DSP_SPACE_P, 0x101, control);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == (form ? 0x101u : 0x102u));
        CHECK(core->registers[dst ? DSP_REG_B2 : DSP_REG_A2] == (expected >> 48));
        CHECK(core->registers[dst ? DSP_REG_B1 : DSP_REG_A1] == ((expected >> 24) & 0xffffff));
        CHECK(core->registers[dst ? DSP_REG_B0 : DSP_REG_A0] == (expected & 0xffffff));
        CHECK((core->registers[DSP_REG_SR] & 0xff00c3) == 0xc000c0);
    }
}

static void check_bit_field_insert(dsp_core_t *core)
{
    const unsigned controls[] = {0,DSP_REG_A1,DSP_REG_B1,DSP_REG_X0,DSP_REG_Y0,DSP_REG_X1,DSP_REG_Y1};
    const unsigned sources[] = {DSP_REG_A0,DSP_REG_B0,DSP_REG_X0,DSP_REG_Y0,DSP_REG_X1,DSP_REG_Y1};
    const unsigned codes[] = {0,1,4,5,6,7};
    const uint64_t patterns[] = {0,0xffffffffffffffULL,0x80000000000000ULL,0x0123456789abcdULL,0xaaaa5555aaaa55ULL};
    for (unsigned dst = 0; dst < 2; dst++)
    for (unsigned form = 0; form < 7; form++)
    for (unsigned src = 0; src < 6; src++)
    for (unsigned width = 1; width <= 24; width++)
    for (unsigned offset = 0; offset + width <= 56; offset++)
    for (unsigned pattern = 0; pattern < 5; pattern++) {
        uint32_t control = (width << 12) | offset;
        unsigned high = dst ? DSP_REG_B2 : DSP_REG_A2;
        unsigned middle = dst ? DSP_REG_B1 : DSP_REG_A1;
        unsigned low = dst ? DSP_REG_B0 : DSP_REG_A0;
        core->registers[sources[src]] = (uint32_t)patterns[4-pattern] & 0xffffff;
        core->registers[high] = (uint32_t)(patterns[pattern] >> 48);
        core->registers[middle] = (uint32_t)(patterns[pattern] >> 24) & 0xffffff;
        core->registers[low] = (uint32_t)patterns[pattern] & 0xffffff;
        if (form) core->registers[controls[form]] = control;
        uint64_t expected = ((uint64_t)core->registers[high] << 48) | ((uint64_t)core->registers[middle] << 24) | core->registers[low];
        uint32_t field = core->registers[sources[src]];
        for (unsigned bit = 0; bit < width; bit++) {
            expected &= ~(1ULL << (offset + bit));
            expected |= ((uint64_t)((field >> bit) & 1)) << (offset + bit);
        }
        core->registers[DSP_REG_SR] = 0xc000c3;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
            (form ? 0x0c1b00 | ((form + 1) << 1) : 0x0c1900) | (codes[src] << 4) | dst);
        if (!form) dsp56k_write_memory(core, DSP_SPACE_P, 0x101, control);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == (form ? 0x101u : 0x102u));
        CHECK(core->registers[high] == (expected >> 48));
        CHECK(core->registers[middle] == ((expected >> 24) & 0xffffff));
        CHECK(core->registers[low] == (expected & 0xffffff));
        CHECK((core->registers[DSP_REG_SR] & 0xff00c3) == 0xc000c0);
    }
}

static void check_half_word_merge(dsp_core_t *core)
{
    const unsigned regs[] = {DSP_REG_A1,DSP_REG_B1,DSP_REG_X0,DSP_REG_Y0,DSP_REG_X1,DSP_REG_Y1};
    const uint32_t sources[] = {0,1,0x7ff,0x800,0xfff,0x1000,0x800000,0xffffff};
    for (unsigned src = 0; src < 6; src++)
    for (unsigned d = 0; d < 2; d++)
    for (unsigned low = 0; low < 4096; low++)
    for (unsigned v = 0; v < 8; v++) {
        unsigned dst = d ? DSP_REG_B1 : DSP_REG_A1;
        core->registers[dst] = 0xab0000 | low;
        core->registers[regs[src]] = sources[v];
        core->registers[d ? DSP_REG_B2 : DSP_REG_A2] = 0x5a;
        core->registers[d ? DSP_REG_B0 : DSP_REG_A0] = 0x123456;
        uint32_t expected = 0;
        for (unsigned bit = 0; bit < 12; bit++) {
            expected |= ((core->registers[dst] >> bit) & 1) << bit;
            expected |= ((core->registers[regs[src]] >> bit) & 1) << (bit + 12);
        }
        core->registers[DSP_REG_SR] = 0xc000ff;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0c1b80 | ((src + 2) << 1) | d);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x101 && core->registers[dst] == expected);
        CHECK(core->registers[d ? DSP_REG_B2 : DSP_REG_A2] == 0x5a);
        CHECK(core->registers[d ? DSP_REG_B0 : DSP_REG_A0] == 0x123456);
        uint32_t flags = 0xc000ff & ~((1u << DSP_SR_N) | (1u << DSP_SR_Z) | (1u << DSP_SR_V));
        if (expected & 0x800000) flags |= 1u << DSP_SR_N;
        if (!expected) flags |= 1u << DSP_SR_Z;
        CHECK(core->registers[DSP_REG_SR] == flags);
    }
    core->registers[DSP_REG_SR] = 0;
    core->registers[DSP_REG_X0] = 5;
    core->registers[DSP_REG_B1] = 11;
    core->registers[DSP_REG_Y0] = 0x15;
    core->registers[DSP_REG_A2] = core->registers[DSP_REG_A1] = core->registers[DSP_REG_A0] = 0;
    core->pc = 0x100;
    dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0c1b89); /* MERGE X0,B */
    dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x0c1b56); /* INSERT B1,Y0,A */
    dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x0c1a06); /* EXTRACT B1,A,A */
    dsp56k_execute_instruction(core);
    CHECK(core->registers[DSP_REG_B1] == 0x500b);
    dsp56k_execute_instruction(core);
    CHECK(core->registers[DSP_REG_A0] == (0x15 << 11));
    dsp56k_execute_instruction(core);
    CHECK(core->pc == 0x103);
    CHECK(core->registers[DSP_REG_A2] == 0xff && core->registers[DSP_REG_A1] == 0xffffff);
    CHECK(core->registers[DSP_REG_A0] == 0xfffff5);
}

static void check_leading_bit_count(dsp_core_t *core)
{
    for (unsigned leading = 1; leading <= 56; leading++)
    for (unsigned sign = 0; sign < 2; sign++)
    for (unsigned src = 0; src < 2; src++)
    for (unsigned dst = 0; dst < 2; dst++)
    for (unsigned tail = 0; tail < 4; tail++) {
        uint64_t value = 0;
        if (leading < 56) {
            unsigned first = 55 - leading;
            uint64_t patterns[] = {0,0xffffffffffffffULL,0x55555555555555ULL,0xaaaaaaaaaaaaaaULL};
            value = (1ULL << first) | (patterns[tail] & ((1ULL << first) - 1));
        }
        if (sign) value ^= 0xffffffffffffffULL;
        int expected = value ? 9 - (int)leading : 0;
        core->registers[src ? DSP_REG_B2 : DSP_REG_A2] = (uint32_t)(value >> 48);
        core->registers[src ? DSP_REG_B1 : DSP_REG_A1] = (uint32_t)(value >> 24) & 0xffffff;
        core->registers[src ? DSP_REG_B0 : DSP_REG_A0] = (uint32_t)value & 0xffffff;
        core->registers[DSP_REG_SR] = 0xc000ff;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0c1e00 | (src << 1) | dst);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x101);
        CHECK(core->registers[dst ? DSP_REG_B2 : DSP_REG_A2] == (expected < 0 ? 0xffu : 0));
        CHECK(core->registers[dst ? DSP_REG_B1 : DSP_REG_A1] == ((uint32_t)expected & 0xffffff));
        CHECK(core->registers[dst ? DSP_REG_B0 : DSP_REG_A0] == 0);
        uint32_t flags = 0xc000ff & ~((1u << DSP_SR_N) | (1u << DSP_SR_Z) | (1u << DSP_SR_V));
        if (expected < 0) flags |= 1u << DSP_SR_N;
        if (!expected) flags |= 1u << DSP_SR_Z;
        CHECK(core->registers[DSP_REG_SR] == flags);
    }
}

static void check_fast_normalization(dsp_core_t *core)
{
    const unsigned regs[] = {DSP_REG_A1,DSP_REG_B1,DSP_REG_X0,DSP_REG_Y0,DSP_REG_X1,DSP_REG_Y1};
    const uint64_t values[] = {0,1,0x7fffffffffffffULL,0x80000000000000ULL,0xffffffffffffffULL,0x123456789abcdeULL};
    for (unsigned src = 0; src < 6; src++)
    for (unsigned d = 0; d < 2; d++)
    for (int count = -55; count <= 56; count++)
    for (unsigned v = 0; v < 6; v++)
    for (unsigned sticky = 0; sticky < 2; sticky++) {
        unsigned high = d ? DSP_REG_B2 : DSP_REG_A2;
        unsigned middle = d ? DSP_REG_B1 : DSP_REG_A1;
        unsigned low = d ? DSP_REG_B0 : DSP_REG_A0;
        core->registers[high] = (uint32_t)(values[v] >> 48);
        core->registers[middle] = (uint32_t)(values[v] >> 24) & 0xffffff;
        core->registers[low] = (uint32_t)values[v] & 0xffffff;
        core->registers[regs[src]] = (uint32_t)count & 0xffffff;
        uint64_t expected = ((uint64_t)core->registers[high] << 48) | ((uint64_t)core->registers[middle] << 24) | core->registers[low];
        bool overflow = false;
        for (int step = 0; step < (count < 0 ? -count : count); step++) {
            unsigned before = (unsigned)(expected >> 55);
            if (count < 0) {
                expected = (expected * 2) & 0xffffffffffffffULL;
                overflow |= before != (expected >> 55);
            } else expected = (expected / 2) | ((uint64_t)before << 55);
        }
        core->registers[DSP_REG_SR] = 0xc00083 | (sticky << DSP_SR_L);
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0c1e20 | ((src + 2) << 1) | d);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x101);
        CHECK(core->registers[high] == (expected >> 48));
        CHECK(core->registers[middle] == ((expected >> 24) & 0xffffff));
        CHECK(core->registers[low] == (expected & 0xffffff));
        CHECK(!!(core->registers[DSP_REG_SR] & (1u << DSP_SR_V)) == overflow);
        CHECK(!!(core->registers[DSP_REG_SR] & (1u << DSP_SR_L)) == (sticky || overflow));
        CHECK((core->registers[DSP_REG_SR] & 0xff0081) == 0xc00081);
    }
    for (unsigned bit = 0; bit < 55; bit++)
    for (unsigned negative = 0; negative < 2; negative++) {
        uint64_t input = 1ULL << bit;
        if (negative) input ^= 0xffffffffffffffULL;
        core->registers[DSP_REG_SR] = 0;
        core->registers[DSP_REG_A2] = (uint32_t)(input >> 48);
        core->registers[DSP_REG_A1] = (uint32_t)(input >> 24) & 0xffffff;
        core->registers[DSP_REG_A0] = (uint32_t)input & 0xffffff;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x0c1e01); /* CLB A,B */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x0c1e26); /* NORMF B1,A */
        dsp56k_execute_instruction(core);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x102);
        CHECK(((core->registers[DSP_REG_A1] >> 23) & 1) == negative);
        CHECK(((core->registers[DSP_REG_A1] >> 22) & 1) != negative);
    }
}

static void check_viterbi_store(dsp_core_t *core)
{
    const uint32_t values[] = {0,1,0x7fffff,0x800000,0xffffff};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned mode = 0; mode < 8; mode++)
    for (unsigned reg = 0; reg < (mode == 6 ? 1u : 8u); reg++)
    for (unsigned src = 0; src < 2; src++)
    for (unsigned bit = 0; bit < 2; bit++)
    for (unsigned v = 0; v < 5; v++) {
        const int update[] = {-3,3,-1,1,0,0,0,-1};
        uint32_t address = mode == 5 ? 0x83 : mode == 7 ? 0x7f : 0x80;
        core->registers[DSP_REG_SR] = 0xc000ff | (compatibility << DSP_SR_SC);
        uint32_t status = core->registers[DSP_REG_SR];
        core->registers[DSP_REG_R0 + reg] = 0x80;
        core->registers[DSP_REG_N0 + reg] = 3;
        core->registers[DSP_REG_M0 + reg] = 0xffffff;
        unsigned high = src ? DSP_REG_B2 : DSP_REG_A2;
        unsigned middle = src ? DSP_REG_B1 : DSP_REG_A1;
        unsigned low = src ? DSP_REG_B0 : DSP_REG_A0;
        core->registers[high] = 0xab;
        core->registers[middle] = values[v];
        core->registers[low] = values[4-v];
        dsp56k_write_memory(core, DSP_SPACE_X, address - 1, 0x123456);
        dsp56k_write_memory(core, DSP_SPACE_Y, address + 1, 0x654321);
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
            0x0ac0c0 | (src << 16) | (mode << 11) | (reg << 8) | (bit << 4));
        if (mode == 6) dsp56k_write_memory(core, DSP_SPACE_P, 0x101, address);
        dsp56k_execute_instruction(core);
        CHECK(core->pc == (mode == 6 ? 0x102u : 0x101u));
        CHECK(core->registers[DSP_REG_R0 + reg] == (uint32_t)(0x80 + update[mode]));
        CHECK(dsp56k_read_memory(core, DSP_SPACE_X, address) == values[v]);
        CHECK(dsp56k_read_memory(core, DSP_SPACE_Y, address) == ((values[4-v] * 2 + bit) & 0xffffff));
        CHECK(dsp56k_read_memory(core, DSP_SPACE_X, address - 1) == 0x123456);
        CHECK(dsp56k_read_memory(core, DSP_SPACE_Y, address + 1) == 0x654321);
        CHECK(core->registers[DSP_REG_SR] == status);
        CHECK(core->registers[high] == 0xab && core->registers[middle] == values[v] && core->registers[low] == values[4-v]);
    }
}

static void check_do_counts(dsp_core_t *core)
{
    const uint32_t counts[] = {0, 1, 0x10001};
    /* Register, short X address, indirect X address and immediate sources. */
    const uint32_t opcodes[] = {0x06c400, 0x060700, 0x066000, 0x060080};
    for (unsigned high = 0; high < 2; high++)
    for (unsigned relative = 0; relative < 2; relative++)
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned form = 0; form < 4; form++)
    for (unsigned c = 0; c < (form == 3 ? 2u : 3u); c++) {
        if (high && (compatibility || c)) continue;
        uint32_t count = counts[c], effective = compatibility ? count & 0xffff : count;
        if (!effective && compatibility) effective = 65536;
        core->registers[DSP_REG_SR] = compatibility << DSP_SR_SC;
        core->registers[DSP_REG_SP] = 0;
        core->registers[DSP_REG_LA] = 0x55;
        core->registers[DSP_REG_LC] = 0x66;
        core->registers[DSP_REG_X0] = count;
        core->registers[DSP_REG_R0] = 7;
        core->registers[DSP_REG_A0] = core->registers[DSP_REG_A1] = core->registers[DSP_REG_A2] = 0;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_X, 7, count);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, opcodes[form] | (relative ? 0x10 : 0) | (form == 3 ? count << 8 : 0));
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, relative ? (high ? 0x10002 : 2) : (high ? 0x10102 : 0x102));
        dsp56k_write_memory(core, DSP_SPACE_P, 0x102, 0x014180);
        dsp56k_execute_instruction(core);
        for (uint32_t i = 0; i < effective; i++) dsp56k_execute_instruction(core);
        CHECK(core->pc == (high ? 0x10103u : 0x103u) && core->registers[DSP_REG_A1] == effective);
        CHECK(core->registers[DSP_REG_SP] == 0 && !(core->registers[DSP_REG_SR] & (1u << DSP_SR_LF)));
        CHECK(core->registers[DSP_REG_LA] == 0x55 && core->registers[DSP_REG_LC] == 0x66);
    }
}

static void check_loop_exit_status(dsp_core_t *core)
{
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned early = 0; early < 2; early++) {
        uint32_t status = 0xc00380 | (compatibility << DSP_SR_SC);
        core->registers[DSP_REG_SR] = status;
        core->registers[DSP_REG_SP] = 0;
        core->registers[DSP_REG_LA] = 0x55;
        core->registers[DSP_REG_LC] = 0x66;
        core->registers[DSP_REG_X0] = 3;
        core->registers[DSP_REG_A0] = core->registers[DSP_REG_A1] = core->registers[DSP_REG_A2] = 0;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x06c400); /* DO X0,$102 */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x102);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x102, early ? 0x00008c : 0x014180);
        dsp56k_execute_instruction(core);
        CHECK(core->registers[DSP_REG_SP] == 2);
        for (unsigned i = 0; i < (early ? 1u : 3u); i++) dsp56k_execute_instruction(core);
        CHECK(core->pc == 0x103 && core->registers[DSP_REG_SP] == 0);
        CHECK(core->registers[DSP_REG_LA] == 0x55 && core->registers[DSP_REG_LC] == 0x66);
        CHECK((core->registers[DSP_REG_SR] & ~0x7fu) == status);
        CHECK(core->registers[DSP_REG_A1] == (early ? 0u : 3u));
    }
}

static void check_linear_agu(dsp_core_t *core)
{
    const uint32_t addresses[] = {0, 0xffff, 0x10000, 0x7fffff, 0xffffff};
    const uint32_t offsets[] = {0, 1, 0xffff, 0x10000, 0x800000, 0xffffff};
    const uint32_t modifiers[] = {0xffff, 0x12ffff, 0xffffff};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned reg = 0; reg < 8; reg++)
    for (unsigned mode = 0; mode < 4; mode++)
    for (unsigned a = 0; a < 5; a++)
    for (unsigned n = 0; n < 6; n++)
    for (unsigned m = 0; m < 3; m++) {
        unsigned destination = (reg + 1) & 7;
        core->registers[DSP_REG_R0 + reg] = addresses[a];
        core->registers[DSP_REG_N0 + reg] = offsets[n];
        core->registers[DSP_REG_M0 + reg] = modifiers[m];
        core->registers[DSP_REG_SR] = (compatibility << DSP_SR_SC) | 0x134;
        core->pc = 0x100;
        /* LUA calculates an address without accessing unmapped high memory. */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           0x044010 | (mode * 8 + reg) << 8 | destination);
        dsp56k_execute_instruction(core);
        int64_t delta = mode == 0 ? -(int64_t)offsets[n] : mode == 1 ? offsets[n] : mode == 2 ? -1 : 1;
        uint32_t expected = (uint32_t)((int64_t)addresses[a] + delta) & (compatibility ? 0xffff : 0xffffff);
        CHECK(core->registers[DSP_REG_R0 + destination] == expected);
        CHECK(core->registers[DSP_REG_R0 + reg] == addresses[a]);
        CHECK(core->registers[DSP_REG_N0 + reg] == offsets[n]);
        CHECK(core->registers[DSP_REG_M0 + reg] == modifiers[m]);
        CHECK(core->registers[DSP_REG_SR] == ((compatibility << DSP_SR_SC) | 0x134));
        CHECK(core->pc == 0x101);
    }
}

static void check_modulo_agu(dsp_core_t *core)
{
    const uint32_t moduli[] = {2, 3, 7, 16, 255, 32768};
    const uint32_t bases[] = {0, 0x8000, 0x120000, 0xff8000};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned reg = 0; reg < 8; reg++)
    for (unsigned mode = 0; mode < 4; mode++)
    for (unsigned m = 0; m < 6; m++)
    for (unsigned a = 0; a < 4; a++)
    for (unsigned position = 0; position < 3; position++)
    for (unsigned n = 0; n < 7; n++) {
        uint32_t mask = compatibility ? 0xffff : 0xffffff;
        uint32_t modulo = moduli[m], block = 1;
        while (block < modulo) block *= 2;
        uint32_t base = bases[a] & mask;
        uint32_t address = base + (position == 0 ? 0 : position == 1 ? modulo / 2 : modulo - 1);
        int32_t offset = n == 0 ? 0 : n == 1 ? 1 : n == 2 ? -1 :
                         n == 3 ? (int32_t)modulo - 1 : n == 4 ? 1 - (int32_t)modulo :
                         n == 5 ? (int32_t)block * 2 : -(int32_t)block * 2;
        if (compatibility && (offset > 32767 || offset < -32768)) continue;
        unsigned destination = (reg + 1) & 7;
        core->registers[DSP_REG_R0 + reg] = address;
        core->registers[DSP_REG_N0 + reg] = ((uint32_t)offset & mask);
        core->registers[DSP_REG_M0 + reg] = (modulo - 1);
        core->registers[DSP_REG_SR] = (compatibility << DSP_SR_SC) | 0x134;
        core->pc = 0x100;
        /* LUA calculates an address without accessing unmapped high memory. */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           0x044010 | (mode * 8 + reg) << 8 | destination);
        dsp56k_execute_instruction(core);
        int32_t delta = mode == 0 ? -offset : mode == 1 ? offset : mode == 2 ? -1 : 1;
        uint32_t expected;
        if (delta % (int32_t)block == 0) {
            expected = (address + delta) & mask;
        } else {
            int32_t relative = (int32_t)(address - base) + delta;
            relative = (relative % (int32_t)modulo + modulo) % modulo;
            expected = (base + relative) & mask;
        }
        CHECK(core->registers[DSP_REG_R0 + destination] == expected);
        CHECK(core->registers[DSP_REG_R0 + reg] == address);
        CHECK(core->registers[DSP_REG_N0 + reg] == ((uint32_t)offset & mask));
        CHECK(core->registers[DSP_REG_M0 + reg] == (modulo - 1));
        CHECK(core->registers[DSP_REG_SR] == ((compatibility << DSP_SR_SC) | 0x134));
        CHECK(core->pc == 0x101);
    }
}

static void check_multiwrap_agu(dsp_core_t *core)
{
    const uint32_t moduli[] = {2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};
    const uint32_t bases[] = {0, 0x8000, 0x120000, 0xff8000};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned reg = 0; reg < 8; reg++)
    for (unsigned mode = 0; mode < 4; mode++)
    for (unsigned m = 0; m < 14; m++)
    for (unsigned a = 0; a < 4; a++)
    for (unsigned position = 0; position < 3; position++)
    for (unsigned n = 0; n < 9; n++) {
        uint32_t mask = compatibility ? 0xffff : 0xffffff;
        uint32_t modulo = moduli[m], block = 1;
        while (block < modulo) block *= 2;
        uint32_t base = bases[a] & mask;
        uint32_t address = base + (position == 0 ? 0 : position == 1 ? modulo / 2 : modulo - 1);
        int32_t offset = n == 0 ? 0 : n == 1 ? 1 : n == 2 ? -1 :
                         n == 3 ? (int32_t)modulo - 1 : n == 4 ? 1 - (int32_t)modulo :
                         n == 5 ? (int32_t)block * 2 : n == 6 ? -(int32_t)block * 2 :
                         n == 7 ? 0x7fffff : -0x800000;
        if (compatibility && (offset > 32767 || offset < -32768)) continue;
        unsigned destination = (reg + 1) & 7;
        core->registers[DSP_REG_R0 + reg] = address;
        core->registers[DSP_REG_N0 + reg] = ((uint32_t)offset & mask);
        core->registers[DSP_REG_M0 + reg] = (0x128000 | (modulo - 1));
        core->registers[DSP_REG_SR] = (compatibility << DSP_SR_SC) | 0x134;
        core->pc = 0x100;
        /* LUA calculates an address without accessing unmapped high memory. */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           0x044010 | (mode * 8 + reg) << 8 | destination);
        dsp56k_execute_instruction(core);
        int32_t delta = mode == 0 ? -offset : mode == 1 ? offset : mode == 2 ? -1 : 1;
        int32_t relative = (int32_t)(address - base) + delta;
        relative = (relative % (int32_t)modulo + modulo) % modulo;
        uint32_t expected = (base + relative) & mask;
        CHECK(core->registers[DSP_REG_R0 + destination] == expected);
        CHECK(core->registers[DSP_REG_R0 + reg] == address);
        CHECK(core->registers[DSP_REG_N0 + reg] == ((uint32_t)offset & mask));
        CHECK(core->registers[DSP_REG_M0 + reg] == (0x128000 | (modulo - 1)));
        CHECK(core->registers[DSP_REG_SR] == ((compatibility << DSP_SR_SC) | 0x134));
        CHECK(core->pc == 0x101);
    }
}

static void check_reverse_agu(dsp_core_t *core)
{
    const uint32_t addresses[] = {0, 0xffff, 0x10000, 0x7fffff, 0xffffff};
    const uint32_t offsets[] = {0, 1, 0xffff, 0x10000, 0x800000, 0xffffff};
    const uint32_t modifiers[] = {0, 0x120000, 0xff0000};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned reg = 0; reg < 8; reg++)
    for (unsigned mode = 0; mode < 4; mode++)
    for (unsigned a = 0; a < 5; a++)
    for (unsigned n = 0; n < 6; n++)
    for (unsigned m = 0; m < 3; m++) {
        unsigned destination = (reg + 1) & 7;
        core->registers[DSP_REG_R0 + reg] = addresses[a];
        core->registers[DSP_REG_N0 + reg] = offsets[n];
        core->registers[DSP_REG_M0 + reg] = modifiers[m];
        core->registers[DSP_REG_SR] = (compatibility << DSP_SR_SC) | 0x134;
        core->pc = 0x100;
        /* LUA calculates an address without accessing unmapped high memory. */
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           0x044010 | (mode * 8 + reg) << 8 | destination);
        dsp56k_execute_instruction(core);
        unsigned width = compatibility ? 16 : 24;
        uint32_t operand = mode < 2 ? offsets[n] : 1;
        uint32_t expected = 0;
        int carry = 0;
        /* Propagate carry/borrow from MSB to LSB, without bit reversal. */
        for (int bit = (int)width - 1; bit >= 0; bit--) {
            int x = (addresses[a] >> bit) & 1;
            int y = (operand >> bit) & 1;
            int result = mode == 0 || mode == 2 ? x - y - carry : x + y + carry;
            expected |= (uint32_t)(result & 1) << bit;
            carry = mode == 0 || mode == 2 ? result < 0 : result > 1;
        }
        CHECK(core->registers[DSP_REG_R0 + destination] == expected);
        CHECK(core->registers[DSP_REG_R0 + reg] == addresses[a]);
        CHECK(core->registers[DSP_REG_N0 + reg] == offsets[n]);
        CHECK(core->registers[DSP_REG_M0 + reg] == modifiers[m]);
        CHECK(core->registers[DSP_REG_SR] == ((compatibility << DSP_SR_SC) | 0x134));
        CHECK(core->pc == 0x101);
    }
}

static void check_immediate_logic(dsp_core_t *core)
{
    const uint32_t values[] = {0, 1, 0x3f, 0x800000, 0xffffff, 0xa55a5a};
    const unsigned operations[] = {6, 2, 3};
    for (unsigned op = 0; op < 3; op++)
    for (unsigned accumulator = 0; accumulator < 2; accumulator++)
    for (unsigned wide = 0; wide < 2; wide++)
    for (unsigned sticky = 0; sticky < 4; sticky++)
    for (unsigned scale = 0; scale < 3; scale++)
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++)
    for (unsigned operand = 0; operand < (wide ? 6u : 64u); operand++) {
        uint32_t immediate = wide ? values[operand] : operand;
        unsigned middle = accumulator ? DSP_REG_B1 : DSP_REG_A1;
        unsigned low = accumulator ? DSP_REG_B0 : DSP_REG_A0;
        unsigned high = accumulator ? DSP_REG_B2 : DSP_REG_A2;
        core->registers[middle] = values[i];
        core->registers[low] = 0x123456;
        core->registers[high] = 0x55;
        uint32_t initial_sr = (0x1234 & ~((3u << DSP_SR_L) | (3u << DSP_SR_S0))) |
                              (sticky << DSP_SR_L) | (scale << DSP_SR_S0);
        core->registers[DSP_REG_SR] = initial_sr;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           (wide ? 0x0140c0 : 0x014080 | immediate << 8) |
                           accumulator << 3 | operations[op]);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, immediate);
        dsp56k_execute_instruction(core);
        uint32_t expected = op == 0 ? values[i] & immediate :
                            op == 1 ? values[i] | immediate : values[i] ^ immediate;
        uint32_t flags = (initial_sr & ~((1u << DSP_SR_N) | (1u << DSP_SR_Z) | (1u << DSP_SR_V))) |
                         ((expected >> 23) << DSP_SR_N) | ((expected == 0) << DSP_SR_Z);
        CHECK(core->registers[middle] == expected && core->registers[DSP_REG_SR] == flags);
        CHECK(core->registers[low] == 0x123456 && core->registers[high] == 0x55);
        CHECK(core->pc == (wide ? 0x102 : 0x101));
    }
}
static void check_long_offset_moves(dsp_core_t *core)
{
    const uint32_t offsets[] = {0, 7, 0xfffff9, 0xffff80};
    const unsigned registers[] = {DSP_REG_X0, DSP_REG_X1, DSP_REG_Y0, DSP_REG_Y1,
                                  DSP_REG_A0, DSP_REG_A1, DSP_REG_B0, DSP_REG_B1};
    for (unsigned compatibility = 0; compatibility < 2; compatibility++)
    for (unsigned space = 0; space < 2; space++)
    for (unsigned reg = 0; reg < 8; reg++)
    for (unsigned r = 0; r < sizeof(registers) / sizeof(registers[0]); r++)
    for (unsigned o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++)
    for (unsigned load = 0; load < 2; load++) {
        uint32_t address = (0x80 + offsets[o]) & 0xffffff;
        core->xram[address] = 0x123456;
        core->yram[address] = 0x654321;
        core->registers[DSP_REG_R0 + reg] = 0x80 | (compatibility ? 0x120000 : 0);
        core->registers[registers[r]] = 0xabcdef;
        core->registers[DSP_REG_SR] = 0x1234 | (compatibility << DSP_SR_SC);
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           0x0a7080 | space << 16 | reg << 8 | load << 6 | registers[r]);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, offsets[o]);
        dsp56k_execute_instruction(core);
        if (core->pc != 0x102 || core->registers[DSP_REG_R0 + reg] != 0x80)
            fprintf(stderr, "MOVE space=%u R=%u data=%u offset=%06X load=%u PC=%06X Rn=%06X opcode=%06X\n",
                    space, reg, registers[r], offsets[o], load, core->pc,
                    core->registers[DSP_REG_R0 + reg], core->cur_inst);
        CHECK(core->pc == 0x102 && core->registers[DSP_REG_R0 + reg] == (0x80 | (compatibility ? 0x120000 : 0)));
        CHECK(core->registers[DSP_REG_SR] == (0x1234 | (compatibility << DSP_SR_SC)));
        if (load) {
            CHECK(core->registers[registers[r]] == (space ? 0x654321 : 0x123456));
            CHECK(core->xram[address] == 0x123456 && core->yram[address] == 0x654321);
        } else {
            CHECK((space ? core->yram : core->xram)[address] == 0xabcdef);
            CHECK((space ? core->xram : core->yram)[address] == (space ? 0x123456 : 0x654321));
        }
    }
}
static void check_indirect_bit_branches(dsp_core_t *core)
{
    const unsigned modes[] = {0, 1, 2, 3, 4, 5, 7};
    for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++)
    for (unsigned reg = 0; reg < 8; reg++)
    for (unsigned space = 0; space < 2; space++)
    for (unsigned branch_set = 0; branch_set < 2; branch_set++)
    for (unsigned jump = 0; jump < 4; jump++)
    for (unsigned bit = 0; bit < 24; bit++)
    for (unsigned set = 0; set < 2; set++) {
        unsigned mode = modes[m];
        uint32_t address = mode == 5 ? 0x83 : mode == 7 ? 0x7f : 0x80;
        uint32_t updated = mode == 0 ? 0x7d : mode == 1 ? 0x83 :
                           mode == 2 || mode == 7 ? 0x7f : mode == 3 ? 0x81 : 0x80;
        core->registers[DSP_REG_R0 + reg] = 0x80;
        core->registers[DSP_REG_N0 + reg] = 3;
        core->registers[DSP_REG_M0 + reg] = 0xffff;
        core->registers[DSP_REG_SR] = 0x1234;
        uint32_t *memory = space ? core->yram : core->xram;
        memory[address] = set << bit;
        core->registers[DSP_REG_SP] = 0;
        core->pc = 0x100;
        dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                           (jump == 0 ? 0x0c8000 : jump == 1 ? 0x0a4080 : jump == 2 ? 0x0b4080 : 0x0d8000) |
                           (mode * 8 + reg) << 8 | space << 6 | branch_set << 5 | bit);
        dsp56k_write_memory(core, DSP_SPACE_P, 0x101, jump && jump != 3 ? 0x200 : 0xfffffc);
        dsp56k_write_memory(core, DSP_SPACE_P, jump == 3 ? 0xfc : 0x200, 0x00000c); /* RTS */
        dsp56k_execute_instruction(core);
        bool taken = set == branch_set;
        CHECK(core->pc == (taken ? (jump && jump != 3 ? 0x200 : 0xfc) : 0x102));
        CHECK(core->registers[DSP_REG_SP] == (taken && jump >= 2 ? 1u : 0u));
        CHECK(core->registers[DSP_REG_R0 + reg] == updated);
        CHECK(core->registers[DSP_REG_SR] == 0x1234);
        CHECK(memory[address] == set << bit);
        if (taken && jump >= 2) {
            dsp56k_execute_instruction(core);
            CHECK(core->pc == 0x102 && core->registers[DSP_REG_SP] == 0);
            CHECK(core->registers[DSP_REG_R0 + reg] == updated);
        }
    }
}
static unsigned transfers;
static DSPDMAState *fault_dma;
static unsigned input_fifo_index;
static bool input_fifo_fault;
static void fifo_rw(void *opaque, uint8_t *ptr, unsigned index, size_t len, bool dir)
{
    DSPDMAState *dma = opaque;
    CHECK(!dir && index < 2 && len <= sizeof(scratch));
    input_fifo_index = index;
    if (input_fifo_fault) { dma->error = true; return; }
    memcpy(ptr, scratch, len);
    transfers++;
}
static uint32_t dma_read(void *core, int space, uint32_t addr)
{
    return dsp56k_read_memory(core, space, addr);
}
static void dma_write(void *core, int space, uint32_t addr, uint32_t value)
{
    dsp56k_write_memory(core, space, addr, value);
}
static void scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    (void)opaque;
    CHECK(addr <= sizeof(scratch) && len <= sizeof(scratch) - addr);
    if (fault_dma && addr == 60) { fault_dma->error = true; return; }
    if (dir) memcpy(scratch + addr, ptr, len);
    else memcpy(ptr, scratch + addr, len);
    transfers++;
}
static void check_dma_memory_edges(dsp_core_t *core)
{
    const uint32_t offsets[] = {0xff9, 0x17f9, 0x1ff9, 0x37f9,
                                0xfff, 0x17ff, 0x1fff, 0x37ff};
    const unsigned formats[] = {1, 2, 6};
    DSPDMAState dma = {0};
    dma.mem_opaque = core;
    dma.mem_read = dma_read; dma.mem_write = dma_write;
    dma.scratch_rw = scratch_rw;
    for (unsigned edge = 0; edge < 8; edge++)
    for (unsigned direction = 0; direction < 2; direction++)
    for (unsigned format_index = 0; format_index < 3; format_index++) {
        uint32_t node_offset = edge < 4 ? offsets[edge] : 0x20;
        uint32_t payload_offset = edge < 4 ? 0x100 : offsets[edge];
        int node_space = node_offset >= 0x2800 ? DSP_SPACE_P : node_offset >= 0x1800 ? DSP_SPACE_Y : DSP_SPACE_X;
        uint32_t node_address = node_offset - (node_space == DSP_SPACE_P ? 0x2800 : node_space == DSP_SPACE_Y ? 0x1800 : 0);
        int space = payload_offset >= 0x2800 ? DSP_SPACE_P : payload_offset >= 0x1800 ? DSP_SPACE_Y : DSP_SPACE_X;
        uint32_t address = payload_offset - (space == DSP_SPACE_P ? 0x2800 : space == DSP_SPACE_Y ? 0x1800 : 0);
        unsigned format = formats[format_index];
        uint32_t descriptor[] = {NODE_POINTER_EOL, (0xf << 5) | format << 10 | direction << 1,
                                 1, payload_offset, 0, 0, 0};
        dsp56k_write_memory(core, space, address, direction ? 0x123456 : 0);
        for (unsigned i = 0; i < 7; i++)
            dsp56k_write_memory(core, node_space, node_address + i, descriptor[i]);
        uint32_t input = format == 1 ? 0x6543 : 0xab654321;
        memcpy(scratch, &input, sizeof(input));
        unsigned before = transfers;
        dma.error = dma.eol = false;
        dsp_dma_write(&dma, DMA_NEXT_BLOCK, node_offset);
        dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
        dsp_dma_step(&dma);
        CHECK(!dma.error && dma.eol && transfers == before + 1);
        if (direction) {
            uint32_t actual = 0;
            memcpy(&actual, scratch, format == 1 ? 2 : 4);
            CHECK(actual == (format == 1 ? 0x1234 : 0x123456));
        } else CHECK(dsp56k_read_memory(core, space, address) == (format == 1 ? 0x654300 : 0x654321));
    }
}

static void failed_boot_read(void *opaque, uint8_t *ptr, uint32_t address, size_t len, bool dir)
{
    DSPState *state = opaque;
    CHECK(!dir && address == 0 && len == 0x800*4);
    memset(ptr, 0, 4); /* Simulate a partial copy followed by a memory fault. */
    state->dma.error = true;
}

static void check_failed_bootstrap(void)
{
    for (unsigned gp = 0; gp < 2; gp++) {
        DSPState *state = dsp_init(NULL, failed_boot_read, NULL, gp != 0);
        CHECK(state);
        state->dma.rw_opaque = state;
        dsp_write_memory(state, 'P', 1, 0x014180);
        dsp_bootstrap(state);
        CHECK(state->core.bootstrap_failed && state->dma.error);
        for (unsigned i = 0; i < 3; i++) {
            dsp_start_frame(state);
            dsp_run(state, 1000);
            CHECK(!dsp_has_work(state) && state->core.pc == 0 && state->core.num_inst == 0);
            CHECK(state->core.registers[DSP_REG_A1] == 0 && !state->dma.eol);
        }
        dsp_reset(state);
        CHECK(!state->core.bootstrap_failed && !state->dma.error);
        dsp_write_memory(state, 'P', 0, 0x014180);
        dsp_write_memory(state, 'P', 1, 0x200);
        dsp_start_frame(state);
        dsp_run(state, 1000);
        CHECK(state->core.registers[DSP_REG_A1] == 1);
        free(state);
    }
}

static void check_debug_runtime(void)
{
    for (unsigned gp = 0; gp < 2; gp++) {
        DSPState *state = dsp_init(NULL, NULL, NULL, gp != 0);
        CHECK(state);
        dsp_write_memory(state, 'P', 0, 0x200);
        dsp_write_memory(state, 'P', 1, 0x014180);
        dsp_start_frame(state);
        dsp_run(state, 1000);
        CHECK(state->core.is_debugging && state->core.pc == 1);
        CHECK(!dsp_has_work(state) && state->core.registers[DSP_REG_A1] == 0);
        unsigned instructions = state->core.num_inst;
        state->dma.control = DMA_CONTROL_RUNNING;
        state->dma.next_block = 0x20;
        dsp56k_add_interrupt(&state->core, DSP_INTER_TRAP);
        dsp_start_frame(state);
        dsp_run(state, 1000);
        CHECK(state->core.is_debugging && !dsp_has_work(state));
        CHECK(state->core.pc == 1 && state->core.num_inst == instructions);
        CHECK(state->dma.next_block == 0x20 && !state->dma.eol);
        dsp_reset(state);
        CHECK(!state->core.is_debugging && state->core.interrupt_counter == 0);
        CHECK(state->dma.control == 0);
        dsp_write_memory(state, 'P', 0, 0x014180);
        dsp_write_memory(state, 'P', 1, 0x200);
        dsp_start_frame(state);
        dsp_run(state, 1000);
        CHECK(state->core.registers[DSP_REG_A1] == 1 && state->core.pc == 2);
        CHECK(state->core.is_debugging);
        free(state);
    }
}

static void check_dma(dsp_core_t *gp)
{
    DSPDMAState dma = {0};
    dma.mem_opaque = gp;
    dma.mem_read = dma_read;
    dma.mem_write = dma_write;
    dma.scratch_rw = scratch_rw;
    dma.fifo_rw = fifo_rw;
    dma.rw_opaque = &dma;
    const uint32_t node[] = { NODE_POINTER_EOL, (0xf << 5) | (6 << 10),
                             2, 0x100, 0, 0, sizeof(scratch) - 1 };
    for (unsigned i = 0; i < 7; i++) gp->xram[0x20 + i] = node[i];
    const uint32_t input[] = {0xab123456, 0xcd654321};
    memcpy(scratch, input, sizeof(input));
    dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0x20);
    dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_FREEZE);
    dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
    uint32_t running_control = dma.control;
    dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_NOP);
    CHECK(dma.control == running_control && !dma.eol && transfers == 0);
    /* Repeated polls while frozen must never clear RUNNING or produce EOL. */
    for (unsigned i = 0; i < 10; i++)
        CHECK(dsp_dma_read(&dma, DMA_CONTROL) & DMA_CONTROL_RUNNING);
    CHECK(!dma.eol && transfers == 0);
    dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_UNFREEZE);
    CHECK(transfers == 0 && !dma.eol);
    for (unsigned i = 0; i < 10; i++)
        CHECK(dsp_dma_read(&dma, DMA_CONTROL) & DMA_CONTROL_RUNNING);
    dsp_dma_step(&dma);
    CHECK(transfers == 1 && dma.eol);
    CHECK(gp->xram[0x100] == 0x123456 && gp->xram[0x101] == 0x654321);
    CHECK(!(dsp_dma_read(&dma, DMA_CONTROL) & DMA_CONTROL_RUNNING));
    CHECK(dsp_dma_read(&dma, DMA_CONTROL) & DMA_CONTROL_STOPPED);

    gp->xram[0x21] |= NODE_CONTROL_DIRECTION;
    gp->xram[0x100] = 0xabcdef;
    dma.eol = false;
    dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0x20);
    dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
    CHECK(transfers == 1 && !dma.eol);
    dsp_dma_step(&dma);
    uint32_t output[2];
    memcpy(output, scratch, sizeof(output));
    CHECK(transfers == 2 && dma.eol);
    CHECK(output[0] == 0xabcdef && output[1] == 0x654321);

    /* Reserved/unsupported buffer routes cannot assert or fake a transfer. */
    for (unsigned direction = 0; direction < 2; direction++)
    for (unsigned buffer = direction ? 4 : 2; buffer < 14; buffer++) {
        unsigned before = transfers;
        gp->xram[0x21] = (buffer << 5) | (6 << 10) | (direction << 1) | 0x10;
        gp->xram[0x24] = 7;
        dma.eol = dma.error = false;
        dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0x20);
        dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
        dsp_dma_step(&dma);
        CHECK(dma.error && !dma.eol && transfers == before);
        CHECK(!(dma.control & DMA_CONTROL_RUNNING) && (dma.control & DMA_CONTROL_STOPPED));
        CHECK(gp->xram[0x24] == 7);
    }
    dma.error = false;
    gp->xram[0x21] = (0xf << 5) | (6 << 10) | NODE_CONTROL_DIRECTION;
    gp->xram[0x24] = 0;

    const uint32_t bad_offsets[] = {0x1000, 0x13ff, 0x2000, 0x27ff, 0x3800, 0xffffff, 0xfff, 0x17ff, 0x1fff, 0x37ff};
    for (unsigned direction = 0; direction < 2; direction++)
    for (unsigned i = 0; i < sizeof(bad_offsets)/sizeof(bad_offsets[0]); i++) {
        unsigned before = transfers;
        gp->xram[0x21] = (0xf << 5) | (6 << 10) | (direction << 1) | 0x10;
        gp->xram[0x23] = bad_offsets[i];
        gp->xram[0x24] = 7;
        dma.eol = dma.error = false;
        dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0x20);
        dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
        dsp_dma_step(&dma);
        CHECK(dma.error && !dma.eol && transfers == before);
        CHECK(!(dma.control & DMA_CONTROL_RUNNING) && (dma.control & DMA_CONTROL_STOPPED));
        CHECK(gp->xram[0x24] == 7);
    }
    dma.error = false;
    gp->xram[0x21] = (0xf << 5) | (6 << 10) | NODE_CONTROL_DIRECTION;
    gp->xram[0x23] = 0x100;
    gp->xram[0x24] = 0;

    const uint32_t bad_nodes[] = {0x1000, 0x13ff, 0xffa, 0x17fa, 0x1ffa, 0x2000, 0x27ff, 0x37fa, 0x3800};
    for (unsigned i = 0; i < sizeof(bad_nodes)/sizeof(bad_nodes[0]); i++) {
        unsigned before = transfers;
        dma.eol = dma.error = false;
        dsp_dma_write(&dma, DMA_NEXT_BLOCK, bad_nodes[i]);
        dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
        dsp_dma_step(&dma);
        CHECK(dma.error && !dma.eol && transfers == before);
        CHECK(dma.next_block == bad_nodes[i]);
        CHECK(!(dma.control & DMA_CONTROL_RUNNING) && (dma.control & DMA_CONTROL_STOPPED));
    }
    dma.error = false;

    /* A descriptor chain remains running between actual transfers. */
    for (unsigned i = 0; i < 7; i++) gp->xram[0x30 + i] = gp->xram[0x20 + i];
    gp->xram[0x20] = 0x30;
    dma.eol = false;
    dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0x20);
    dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
    dsp_dma_step(&dma);
    CHECK(transfers == 3 && !dma.eol && dma.next_block == 0x30);
    CHECK(dsp_dma_read(&dma, DMA_CONTROL) & DMA_CONTROL_RUNNING);
    dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_FREEZE);
    dsp_dma_step(&dma);
    CHECK(transfers == 3 && !dma.eol);
    dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_UNFREEZE);
    dsp_dma_step(&dma);
    CHECK(transfers == 4 && dma.eol);
    CHECK(!(dsp_dma_read(&dma, DMA_CONTROL) & DMA_CONTROL_RUNNING));
    /* The final two P words are a valid half-open memory range. */
    gp->xram[0x20] = NODE_POINTER_EOL;
    gp->xram[0x21] = (0xf << 5) | (6 << 10);
    gp->xram[0x23] = 0x37fe;
    memcpy(scratch, input, sizeof(input));
    dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0x20);
    dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
    dsp_dma_step(&dma);
    CHECK(gp->pram[0xffe] == 0x123456 && gp->pram[0xfff] == 0x654321);
    /* Encoded count 0x11 means one block, two channels: two words, not 17. */
    gp->xram[0x21] |= NODE_CONTROL_DIRECTION | 1;
    gp->xram[0x22] = 0x11;
    memset(scratch, 0, sizeof(scratch));
    dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0x20);
    dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
    dsp_dma_step(&dma);
    memcpy(output, scratch, sizeof(output));
    CHECK(output[0] == 0x123456 && output[1] == 0x654321);
    /* Both hardware input FIFOs feed DSP memory through the existing callback. */
    for (unsigned index = 0; index < 2; index++) {
        gp->xram[0x21] = (index << 5) | (1 << 10);
        gp->xram[0x22] = 2;
        gp->xram[0x23] = 0x100;
        const uint16_t samples[] = {0x8001, 0x7ffe};
        memcpy(scratch, samples, sizeof(samples));
        dma.eol = false;
        dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0x20);
        dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
        dsp_dma_step(&dma);
        CHECK(input_fifo_index == index && dma.eol && !dma.error);
        CHECK(gp->xram[0x100] == 0x800100 && gp->xram[0x101] == 0x7ffe00);
    }
    /* Interleaved host samples become planar DSP channels in both formats. */
    for (unsigned format = 1; format <= 6; format += 5)
    for (unsigned buffer = 0; buffer < 2; buffer++)
    for (unsigned space = 0; space < 3; space++)
    for (unsigned channels = 1; channels <= 3; channels++)
    for (unsigned blocks = 1; blocks <= 4; blocks += 3) {
        const uint32_t ends[] = {0x1000, 0x2000, 0x3800};
        const uint32_t bases[] = {0, 0x1800, 0x2800};
        uint32_t words = channels * blocks;
        uint32_t offset = ends[space] - words;
        uint32_t address = offset - bases[space];
        gp->xram[0x21] = 1 | ((buffer ? 0xf : 0) << 5) | (format << 10);
        gp->xram[0x22] = (blocks << 4) | (channels - 1);
        gp->xram[0x23] = offset;
        for (unsigned i = 0; i < words; i++) {
            uint32_t sample = (0x8001 + i * 0x311) & 0xffff;
            if (format == 1) {
                uint16_t packed = (uint16_t)sample;
                memcpy(scratch + i * 2, &packed, 2);
            } else {
                sample = (sample << 8) | 0xab000000;
                memcpy(scratch + i * 4, &sample, 4);
            }
            dsp56k_write_memory(gp, space, address + i, 0xdeadbe);
        }
        dma.eol = false;
        dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0x20);
        dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
        dsp_dma_step(&dma);
        CHECK(dma.eol && !dma.error);
        for (unsigned channel = 0; channel < channels; channel++)
        for (unsigned block = 0; block < blocks; block++) {
            uint32_t expected = ((0x8001 + (block * channels + channel) * 0x311) & 0xffff) << 8;
            CHECK(dsp56k_read_memory(gp, space, address + channel * blocks + block) == expected);
        }
    }
    /* Failure must preserve every destination word, including planar input. */
    gp->xram[0x21] = 1 | (1 << 10);
    gp->xram[0x22] = (4 << 4) | 2;
    gp->xram[0x23] = 0x100;
    for (unsigned i = 0; i < 12; i++) gp->xram[0x100 + i] = 0x123456 + i;
    input_fifo_fault = true;
    dma.eol = false;
    gp->xram[0x100] = 0x123456;
    dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0x20);
    dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
    dsp_dma_step(&dma);
    CHECK(dma.error && !dma.eol);
    for (unsigned i = 0; i < 12; i++) CHECK(gp->xram[0x100 + i] == 0x123456 + i);
    input_fifo_fault = false;
    dma.error = false;
    /* DMA-uploaded P words must replace already decoded guest instructions. */
    dsp56k_write_memory(gp, DSP_SPACE_P, 0, 0x014180); /* add #1,A */
    gp->registers[DSP_REG_A0] = gp->registers[DSP_REG_A1] = gp->registers[DSP_REG_A2] = 0;
    gp->pc = 0;
    dsp56k_execute_instruction(gp);
    CHECK(gp->registers[DSP_REG_A1] == 1);
    uint32_t replacement = 0x014284; /* sub #2,A */
    memcpy(scratch, &replacement, sizeof(replacement));
    gp->xram[0x21] = (0xf << 5) | (6 << 10);
    gp->xram[0x22] = 1;
    gp->xram[0x23] = 0x2800;
    dma.eol = false;
    dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0x20);
    dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
    dsp_dma_step(&dma);
    CHECK(dma.eol && !dma.error);
    gp->pc = 0;
    dsp56k_execute_instruction(gp);
    CHECK(gp->registers[DSP_REG_A1] == 0xffffff && gp->registers[DSP_REG_A2] == 0xff);
    gp->xram[0x23] = 0x37fe;
    /* A failed tail chunk must not continue into the wrapped head chunk. */
    gp->xram[0x21] = (0xe << 5) | (6 << 10) | NODE_CONTROL_DIRECTION | (1 << 4);
    gp->xram[0x22] = 2;
    gp->xram[0x24] = 60;
    memset(scratch, 0x5a, sizeof(scratch));
    dma.eol = false;
    fault_dma = &dma;
    unsigned before_fault = transfers;
    dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0x20);
    dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
    dsp_dma_step(&dma);
    fault_dma = NULL;
    CHECK(dma.error && !dma.eol && transfers == before_fault);
    CHECK(scratch[0] == 0x5a && gp->xram[0x24] == 60);
    /* Unsupported packing must fail before transfer/writeback/completion. */
    const unsigned unsupported[] = {0, 3 << 10, 4 << 10, 5 << 10, 7 << 10,
                                    (1 << 10) | (1 << 2),
                                    (1 << 10) | (1 << 3),
                                    (1 << 10) | (1 << 13)};
    for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
        gp->xram[0x20] = NODE_POINTER_EOL;
        gp->xram[0x21] = NODE_CONTROL_DIRECTION | (1 << 4) | (0xf << 5) | unsupported[i];
        gp->xram[0x22] = 2;
        gp->xram[0x23] = 0x100;
        gp->xram[0x24] = 60;
        dma.error = dma.eol = false;
        dma.control = 0;
        dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0x20);
        dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
        dsp_dma_step(&dma);
        CHECK(dma.error && !dma.eol && transfers == before_fault);
        CHECK(!(dma.control & DMA_CONTROL_RUNNING) && (dma.control & DMA_CONTROL_STOPPED));
        CHECK(scratch[0] == 0x5a && gp->xram[0x24] == 60);
    }
}

#endif

static uint32_t replay_event[3];
static uint32_t replay_read(dsp_core_t *core, uint32_t address)
{
    (void)core;
    replay_event[0] = 1; replay_event[1] = address;
    return 0;
}
static void replay_write(dsp_core_t *core, uint32_t address, uint32_t value)
{
    (void)core;
    replay_event[0] = 2; replay_event[1] = address; replay_event[2] = value;
}
static int replay_prefix(const char *input, const char *output, bool gp)
{
    dsp_core_t *core = calloc(1, sizeof(*core));
    CHECK(core);
    core->is_gp = gp;
    memset(core->pram, 0xca, sizeof(core->pram));
    memset(core->xram, 0xca, sizeof(core->xram));
    memset(core->yram, 0xca, sizeof(core->yram));
    dsp56k_reset_cpu(core);
    FILE *file = fopen(input, "rb");
    CHECK(file && fread(core->pram, 4, 0x800, file) == 0x800);
    CHECK(fgetc(file) == EOF);
    fclose(file);
    core->read_peripheral = replay_read;
    core->write_peripheral = replay_write;
    unsigned steps = 0;
    /* Stop at the first external input/output: subsequent execution requires
     * captured peripheral and DMA state. No substitute completion is supplied. */
    while (!replay_event[0] && steps < 10000) {
        dsp56k_execute_instruction(core);
        steps++;
    }
    CHECK(replay_event[0]);
    file = fopen(output, "wb");
    CHECK(file);
#define SAVE(field) CHECK(fwrite(&(field), 1, sizeof(field), file) == sizeof(field))
    SAVE(steps); SAVE(core->pc); SAVE(core->registers); SAVE(core->stack);
    SAVE(core->xram); SAVE(core->yram); SAVE(core->pram); SAVE(core->mixbuffer);
    SAVE(replay_event);
#undef SAVE
    CHECK(fclose(file) == 0);
    printf("%s prefix: %u instructions, PC=%06X event=%u address=%06X value=%06X\n",
           gp ? "GP" : "EP", steps, core->pc, replay_event[0], replay_event[1], replay_event[2]);
    free(core);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 4) return replay_prefix(argv[1], argv[2], strcmp(argv[3], "gp") == 0);
#ifndef DSP_REFERENCE_CPU_ONLY
    if (argc == 2 && !strcmp(argv[1], "stack-errors")) {
        dsp_core_t *core = calloc(1, sizeof(*core)); CHECK(core);
        core->is_gp = true;
        check_stack_error_transition(core);
        check_stack_fault_latching(core);
        check_stack_move_width(core);
        check_stack_memory_moves(core);
        check_extended_stack(core);
        check_extended_stack_faults(core);        check_extended_fault_rearm(core);
        check_extended_calls(core);
        check_extended_loop_stack(core);
        check_extended_sp_move(core);
        check_stack_enable(core);
        check_normal_stack_count(core);
        check_extended_sc_refill(core);
        check_extended_double_spill(core);
        check_stack_error_delivery(core);
        free(core);
        puts("Guest stack errors delivered and repaired without host abort");
        return 0;
    }
#endif
    CHECK(argc == 1);
    dsp_core_t *gp = calloc(1, sizeof(*gp));
    dsp_core_t *ep = calloc(1, sizeof(*ep));
    CHECK(gp && ep);
    gp->is_gp = true;
    for (unsigned reg = 0; reg < 8; reg++) {
        gp->registers[DSP_REG_M0 + reg] = 0x123456;
        ep->registers[DSP_REG_M0 + reg] = 0x654321;
    }
    dsp56k_reset_cpu(gp);
    dsp56k_reset_cpu(ep);
#ifndef DSP_REFERENCE_CPU_ONLY
    for (unsigned reg = 0; reg < 8; reg++) {
        CHECK(gp->registers[DSP_REG_M0 + reg] == 0xffffff);
        CHECK(ep->registers[DSP_REG_M0 + reg] == 0xffffff);
    }
#endif

    /* Execute uploaded words, then replace an already decoded instruction. */
    dsp56k_write_memory(gp, DSP_SPACE_P, 0, 0x014180); /* add #1,A */
    dsp56k_write_memory(gp, DSP_SPACE_P, 1, 0x0c0000); /* jmp 0 */
    dsp56k_execute_instruction(gp);
    CHECK(gp->pc == 1 && gp->registers[DSP_REG_A1] == 1);
    dsp56k_execute_instruction(gp);
    CHECK(gp->pc == 0);
    dsp56k_write_memory(gp, DSP_SPACE_P, 0, 0x014284); /* sub #2,A */
    dsp56k_execute_instruction(gp);
    CHECK(gp->pc == 1 && gp->registers[DSP_REG_A1] == 0xffffff);
    CHECK(gp->registers[DSP_REG_A2] == 0xff);
    CHECK(ep->pc == 0 && ep->registers[DSP_REG_A1] == 0);

    dsp56k_write_memory(gp, DSP_SPACE_X, 7, 0x123456);
    dsp56k_write_memory(gp, DSP_SPACE_Y, 7, 0x654321);
    CHECK(dsp56k_read_memory(gp, DSP_SPACE_X, 7) == 0x123456);
    CHECK(dsp56k_read_memory(gp, DSP_SPACE_Y, 7) == 0x654321);
    CHECK(dsp56k_read_memory(ep, DSP_SPACE_X, 7) == 0);
    check_polling_instructions(gp);
#ifndef DSP_REFERENCE_CPU_ONLY
    check_compatibility_peripheral_window(gp);
    check_lower_io_bits(gp);
    check_lower_io_register_moves(gp);
    check_lower_io_program_moves(gp);
    check_lower_io_program_upload(gp);
    check_io_bit_branches(gp);
    check_indirect_bit_branches(gp);
    check_long_offset_moves(gp);
    check_immediate_logic(gp);
    check_io_bit_jumps(gp);
    check_register_branches(gp);
    check_register_bit_calls(gp);
    check_immediate_shifts(gp);
    check_agu_register_loads(gp);
    check_movec_agu_source(gp);
    check_move_agu_source(gp);
    check_parallel_memory_agu_source(gp);
    check_status_register_writes(gp);
    check_status_register_reads(gp);
    check_loop_register_transfers(gp);
    check_wide_repeat_counts(gp);
    check_repeat_stack_source(gp);
    check_loop_exit_status(gp);
    check_do_counts(gp);
    check_loop_controller_sources(gp);
    check_wide_stack(gp);
    check_interrupt_return_stack(gp);
    check_forever_loops(gp);
    check_nested_loop_flags(gp);
    check_conditional_loop_exit(gp);
    check_control_byte_logic(gp);
    check_operating_mode_moves(gp);
    check_vector_register_moves(gp);
    check_vector_relocation(gp);
    check_stop_execution(gp);
    check_wait_execution(gp);
    check_disabled_cache_interrupts(gp);
    check_stack_extension_registers(gp);
    check_stack_error_transition(gp);
    check_stack_fault_latching(gp);
    check_stack_move_width(gp);
    check_stack_memory_moves(gp);
    check_extended_stack(gp);
    check_extended_stack_faults(gp);
    check_extended_fault_rearm(gp);
    check_extended_calls(gp);
    check_extended_loop_stack(gp);
    check_extended_sp_move(gp);
    check_stack_enable(gp);
    check_normal_stack_count(gp);
    check_extended_sc_refill(gp);
    check_extended_double_spill(gp);
    check_stack_error_delivery(gp);
    check_trap_delivery(gp);
    check_repeat_fetch_and_interrupt(gp);
    check_immediate_accumulate(gp);
    check_rounding_modes(gp);
    check_mixed_multiply(gp);
    check_bit_field_extract(gp);
    check_bit_field_insert(gp);
    check_half_word_merge(gp);
    check_leading_bit_count(gp);
    check_fast_normalization(gp);
    check_viterbi_store(gp);
    check_relative_address_loads(gp);
    check_offset_address_loads(gp);
    check_arithmetic_right_shifts(gp);
    check_arithmetic_left_shifts(gp);
    check_linear_agu(gp);
    check_reverse_agu(gp);
    check_modulo_agu(gp);
    check_multiwrap_agu(gp);
    for (unsigned action = DMA_CONTROL_ACTION_ABORT; action <= DMA_CONTROL_ACTION; action++) {
        for (unsigned complete = 0; complete < 2; complete++) {
            DSPDMAState dma = {0};
            dma.control = DMA_CONTROL_RUNNING | DMA_CONTROL_FROZEN;
            dma.configuration = 0x123;
            dma.next_block = 0x20;
            dma.start_block = 0x40;
            dma.eol = complete != 0;
            dsp_dma_write(&dma, DMA_CONTROL, action);
            CHECK(dma.error && !(dma.control & DMA_CONTROL_RUNNING));
            CHECK((dma.control & (DMA_CONTROL_STOPPED | DMA_CONTROL_FROZEN)) ==
                  (DMA_CONTROL_STOPPED | DMA_CONTROL_FROZEN));
            dsp_dma_step(&dma); /* Null callbacks prove no payload access. */
            CHECK(dma.eol == (complete != 0));
            CHECK(dma.next_block == 0x20 && dma.start_block == 0x40 && dma.configuration == 0x123);
        }
    }
    check_failed_bootstrap();
    check_debug_runtime();
    check_dma(gp);
    check_dma_memory_edges(gp);
#endif
    free(gp);
    free(ep);
    puts("DSP CPU execution, opcode replacement and GP/EP isolation passed");
    return 0;
}
