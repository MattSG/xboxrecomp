/* Exercise the private trace formatter without adding a runtime API. */
#include "../../src/apu/dsp/interp/dsp_cpu.c"

static uint32_t move_bus_value;
static uint32_t move_bus_read(dsp_core_t *core, uint32_t address)
{
    (void)core; (void)address; return move_bus_value;
}
static void move_bus_write(dsp_core_t *core, uint32_t address, uint32_t value)
{
    (void)core; (void)address; move_bus_value = value;
}

/* Independent accumulator model: SA uses a continuous 40-bit value whose
 * significant words sit in bits 23:8; normal mode is a 56-bit value. */
static int64_t wrap_bits(int64_t value, unsigned bits)
{
    uint64_t raw = (uint64_t)value & ((1ULL << bits) - 1);
    return (int64_t)raw - ((raw >> (bits - 1)) ? (int64_t)(1ULL << bits) : 0);
}

static void set_acc(dsp_core_t *core, unsigned acc, int64_t value, bool sa)
{
    uint64_t raw = (uint64_t)value;
    core->registers[DSP_REG_A2+acc] = sa ? (raw >> 32) & 0xff : (raw >> 48) & 0xff;
    core->registers[DSP_REG_A1+acc] = sa ? (((raw >> 16) & 0xffff) << 8) | 0x55 : (raw >> 24) & 0xffffff;
    core->registers[DSP_REG_A0+acc] = sa ? ((raw & 0xffff) << 8) | 0xaa : raw & 0xffffff;
}

static int64_t get_acc(const dsp_core_t *core, unsigned acc, bool sa)
{
    uint64_t raw = sa ? ((uint64_t)core->registers[DSP_REG_A2+acc] << 32) |
                        ((uint64_t)(core->registers[DSP_REG_A1+acc] >> 8) << 16) |
                        (core->registers[DSP_REG_A0+acc] >> 8)
                      : ((uint64_t)core->registers[DSP_REG_A2+acc] << 48) |
                        ((uint64_t)core->registers[DSP_REG_A1+acc] << 24) |
                        core->registers[DSP_REG_A0+acc];
    return wrap_bits((int64_t)raw, sa ? 40 : 56);
}

/* SA results must leave the ignored low bytes clear. */
static bool acc_is(const dsp_core_t *core, unsigned acc, int64_t value, bool sa)
{
    if (sa && ((core->registers[DSP_REG_A1+acc] | core->registers[DSP_REG_A0+acc]) & 0xff)) return false;
    return get_acc(core, acc, sa) == wrap_bits(value, sa ? 40 : 56);
}

static void run_at_100(dsp_core_t *core, uint32_t opcode, uint32_t extension)
{
    core->pc = 0x100;
    dsp56k_write_memory(core, DSP_SPACE_P, 0x100, opcode);
    dsp56k_write_memory(core, DSP_SPACE_P, 0x101, extension);
    dsp56k_execute_instruction(core);
}

static bool sr_bit(const dsp_core_t *core, unsigned bit)
{
    return (core->registers[DSP_REG_SR] >> bit) & 1;
}

static int64_t floor_shift(int64_t value, unsigned n)
{
    return value >= 0 ? value >> n : -((-value + ((1LL << n) - 1)) >> n);
}

static int check_sa_alu_frontier(dsp_core_t *core)
{
    /* MAX/MAXM (DSP56300FM 13-106/107): if B-A <= 0 (|B|-|A|) then A -> B,
     * C cleared on transfer. Both widths. */
    for (unsigned sa = 0; sa < 2; sa++) {
        unsigned bits = sa ? 40 : 56;
        int64_t top = (int64_t)(1ULL << (bits - 1)) - 1, bottom = -top - 1;
        const int64_t values[] = {0, 1, -1, 0x12345678, -0x40000000, top, bottom};
        for (unsigned magnitude = 0; magnitude < 2; magnitude++)
            for (unsigned i = 0; i < 7; i++)
                for (unsigned j = 0; j < 7; j++) {
                    dsp56k_reset_cpu(core);
                    core->registers[DSP_REG_SR] = sa << DSP_SR_SA;
                    set_acc(core, 0, values[i], sa);
                    set_acc(core, 1, values[j], sa);
                    uint32_t a2 = core->registers[DSP_REG_A2], a1 = core->registers[DSP_REG_A1],
                             a0 = core->registers[DSP_REG_A0], b2 = core->registers[DSP_REG_B2],
                             b1 = core->registers[DSP_REG_B1], b0 = core->registers[DSP_REG_B0];
                    int64_t a = values[i], b = values[j];
                    if (magnitude) {
                        a = a < 0 ? wrap_bits(-a, bits) : a;
                        b = b < 0 ? wrap_bits(-b, bits) : b;
                    }
                    bool transfer = wrap_bits(b - a, bits) <= 0;
                    run_at_100(core, magnitude ? 0x200015 : 0x20001d, 0);
                    bool moved = core->registers[DSP_REG_B2] == a2 && core->registers[DSP_REG_B1] == a1 &&
                                 core->registers[DSP_REG_B0] == a0;
                    bool kept = core->registers[DSP_REG_B2] == b2 && core->registers[DSP_REG_B1] == b1 &&
                                core->registers[DSP_REG_B0] == b0;
                    if (!(transfer ? moved : kept) || sr_bit(core, DSP_SR_C) == transfer ||
                        core->registers[DSP_REG_A1] != a1 || core->pc != 0x101) {
                        fprintf(stderr, "max sa=%u magnitude=%u i=%u j=%u\n", sa, magnitude, i, j);
                        return 1;
                    }
                }
    }
    /* SA single-operand arithmetic on the continuous 40-bit value. */
    const int64_t sa_values[] = {0, 1, -1, 0xffff, 0x10000, 0x7fffffffffLL, -0x8000000000LL, -0x12345678LL};
    for (unsigned acc = 0; acc < 2; acc++)
        for (unsigned i = 0; i < 8; i++) {
            int64_t v = sa_values[i];
            const struct { uint32_t opcode; int64_t result; bool overflow; unsigned carry; } cases[] = {
                {0x200026 | (acc << 3), v < 0 ? wrap_bits(-v, 40) : v, v == -0x8000000000LL, 2},
                {0x200036 | (acc << 3), wrap_bits(-v, 40), v == -0x8000000000LL, 2},
                {0x000008 | acc, v + 1, v == 0x7fffffffffLL, v == -1},
                {0x00000a | acc, v - 1, v == -0x8000000000LL, v == 0},
            };
            for (unsigned c = 0; c < 4; c++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
                set_acc(core, acc, v, true);
                run_at_100(core, cases[c].opcode, 0);
                if (!acc_is(core, acc, cases[c].result, true) ||
                    sr_bit(core, DSP_SR_V) != cases[c].overflow ||
                    (cases[c].carry != 2 && sr_bit(core, DSP_SR_C) != cases[c].carry)) {
                    fprintf(stderr, "sa unary case=%u acc=%u i=%u\n", c, acc, i);
                    return 1;
                }
            }
        }
    /* SA immediates follow the bus mapping: integers align to bit 8. */
    const uint32_t longs[] = {0, 1, 0x7fff, 0x8000, 0xabcdef};
    const uint32_t ops[3] = {0, 4, 5}; /* ADD, SUB, CMP */
    for (unsigned acc = 0; acc < 2; acc++)
        for (unsigned i = 0; i < 8; i++)
            for (unsigned op = 0; op < 3; op++)
                for (unsigned form = 0; form < 2; form++)
                    for (unsigned k = 0; k < 5; k++) {
                        uint32_t value = form ? longs[k] : (uint32_t)(k * 15);
                        int64_t source = form ? (int64_t)(int16_t)(value & 0xffff) * 65536 : (int64_t)value * 65536;
                        int64_t v = sa_values[i];
                        int64_t wrapped = wrap_bits(op == 0 ? v + source : v - source, 40);
                        dsp56k_reset_cpu(core);
                        core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
                        set_acc(core, acc, v, true);
                        run_at_100(core, (form ? 0x0140c0 : 0x014080 | (value << 8)) | ops[op] | (acc << 3), value);
                        bool ok = op == 2 ? get_acc(core, acc, true) == v : acc_is(core, acc, wrapped, true);
                        if (!ok || sr_bit(core, DSP_SR_Z) != (wrapped == 0) || sr_bit(core, DSP_SR_N) != (wrapped < 0) ||
                            core->pc != 0x101u + form) {
                            fprintf(stderr, "sa immediate op=%u form=%u acc=%u i=%u k=%u\n", op, form, acc, i, k);
                            return 1;
                        }
                    }
    /* Word immediates for CMP are sign-extended (DSP56300FM 13-45). */
    dsp56k_reset_cpu(core);
    run_at_100(core, 0x0140c5, 0x800000);
    if (sr_bit(core, DSP_SR_N) || sr_bit(core, DSP_SR_Z) || core->registers[DSP_REG_A1] != 0) {
        fprintf(stderr, "cmp long immediate sign extension\n");
        return 1;
    }
    /* SA DIV: the quotient bit enters the 40-bit LSB (A0 bit 8). */
    for (unsigned carry = 0; carry < 2; carry++)
        for (unsigned i = 0; i < 8; i++)
            for (unsigned j = 0; j < 5; j++) {
                const int32_t words[] = {1, -1, 0x4000, -0x8000, 0x1234};
                int64_t d = sa_values[i], s = (int64_t)words[j] * 65536;
                bool differ = (d < 0) != (words[j] < 0);
                int64_t expected = wrap_bits(d * 2 + (differ ? s : -s), 40) | carry;
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = (1u << DSP_SR_SA) | carry;
                core->registers[DSP_REG_X0] = (((uint32_t)words[j] & 0xffff) << 8) | 0x77;
                set_acc(core, 0, d, true);
                run_at_100(core, 0x018040, 0);
                if (!acc_is(core, 0, expected, true) || sr_bit(core, DSP_SR_C) != (expected >= 0)) {
                    fprintf(stderr, "sa div carry=%u i=%u j=%u\n", carry, i, j);
                    return 1;
                }
            }
    /* SA CLB counts the 40-bit value; NORMF consumes its 16-bit result. */
    for (unsigned i = 0; i < 8; i++) {
        int64_t v = sa_values[i];
        unsigned count = 0;
        for (int bit = 39; bit >= 0 && (((uint64_t)v >> bit) & 1) == (((uint64_t)v >> 39) & 1); bit--) count++;
        int result = v ? 9 - (int)count : 0;
        dsp56k_reset_cpu(core);
        core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
        set_acc(core, 0, v, true);
        run_at_100(core, 0x0c1e01, 0); /* CLB A,B */
        if (core->registers[DSP_REG_B1] != (((uint32_t)result & 0xffff) << 8) || core->registers[DSP_REG_B0] ||
            core->registers[DSP_REG_B2] != (result < 0 ? 0xffu : 0)) {
            fprintf(stderr, "sa clb i=%u\n", i);
            return 1;
        }
        run_at_100(core, 0x0c1e26, 0); /* NORMF B1,A */
        int64_t normalized = result < 0 ? wrap_bits(v * ((int64_t)1 << -result), 40) : floor_shift(v, (unsigned)result);
        if (!acc_is(core, 0, normalized, true)) {
            fprintf(stderr, "sa normf i=%u\n", i);
            return 1;
        }
    }
    /* SA register shift counts use the control word's bit-8 LSB. */
    for (unsigned count = 0; count < 20; count++)
        for (unsigned i = 0; i < 8; i++) {
            int64_t v = sa_values[i];
            dsp56k_reset_cpu(core);
            core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
            core->registers[DSP_REG_X0] = (count << 8) | 0x07;
            set_acc(core, 0, v, true);
            run_at_100(core, 0x0c1e48, 0); /* ASL X0,A,A */
            if (!acc_is(core, 0, v * ((int64_t)1 << count), true)) {
                fprintf(stderr, "sa asl register count=%u i=%u\n", count, i);
                return 1;
            }
            dsp56k_reset_cpu(core);
            core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
            core->registers[DSP_REG_X0] = (count << 8) | 0x07;
            core->registers[DSP_REG_A1] = 0x9abc55;
            run_at_100(core, 0x0c1e18, 0); /* LSL X0,A */
            if (core->registers[DSP_REG_A1] != (((0x9abcu << count) & 0xffff) << 8)) {
                fprintf(stderr, "sa lsl register count=%u\n", count);
                return 1;
            }
        }
    /* SA immediate, power-of-two and mixed-sign multiplies use 16-bit operands. */
    for (unsigned i = 0; i < 5; i++)
        for (unsigned j = 0; j < 5; j++) {
            const int32_t words[] = {0, 1, -1, 0x7fff, -0x8000};
            uint32_t x0 = (((uint32_t)words[i] & 0xffff) << 8) | 0x33;
            dsp56k_reset_cpu(core);
            core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
            core->registers[DSP_REG_X0] = x0;
            run_at_100(core, 0x0141c0, (uint32_t)words[j] & 0xffffff); /* MPYI #j,X0,A */
            if (!acc_is(core, 0, (int64_t)words[i] * words[j] * 2, true)) {
                fprintf(stderr, "sa mpyi i=%u j=%u\n", i, j);
                return 1;
            }
            for (unsigned uu = 0; uu < 2; uu++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
                core->registers[DSP_REG_X0] = x0;
                run_at_100(core, 0x012780 | (uu << 6), 0); /* MPY su/uu X0,X0,A */
                int64_t left = uu ? (words[i] & 0xffff) : words[i];
                if (!acc_is(core, 0, left * (words[i] & 0xffff) * 2, true)) {
                    fprintf(stderr, "sa mixed uu=%u i=%u\n", uu, i);
                    return 1;
                }
            }
            unsigned n = j * 4 + 1;
            dsp56k_reset_cpu(core);
            core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
            core->registers[DSP_REG_X0] = x0;
            run_at_100(core, 0x0100d0 | (n << 8), 0); /* MPY X0,#n,A */
            if (!acc_is(core, 0, floor_shift((int64_t)words[i] * 65536, n), true)) {
                fprintf(stderr, "sa mpy power i=%u n=%u\n", i, n);
                return 1;
            }
        }
    /* NORM updates a 24-bit address register. */
    dsp56k_reset_cpu(core);
    core->registers[DSP_REG_SR] = 1u << DSP_SR_E;
    core->registers[DSP_REG_R0] = 0x00ffff;
    set_acc(core, 0, 0x12345678LL << 24, false);
    run_at_100(core, 0x01d815, 0);
    if (core->registers[DSP_REG_R0] != 0x010000) {
        fprintf(stderr, "norm address width\n");
        return 1;
    }
    return 0;
}

int main(void)
{
    dsp_core_t *core = calloc(1, sizeof(*core));
    const char *names[] = { "mr", "ccr", "com", "eom" };
    if (!core) return 1;
    dsp56k_reset_cpu(core);
    for (unsigned op = 0; op < 2; op++)
        for (unsigned target = 0; target < 4; target++)
            for (unsigned immediate = 0; immediate < 256; immediate++) {
                char expected[32];
                core->pc = 0x100;
                core->pram[0x100] = (op ? 0xf8 : 0xb8) | (immediate << 8) | target;
                strcpy(core->disasm_str_instr, "stale");
                sprintf(expected, "%s #$%02x,%s", op ? "ori" : "andi", immediate, names[target]);
                if (disasm_instruction(core, DSP_DISASM_MODE) != 1 ||
                    strcmp(core->disasm_str_instr, expected) || core->pc != 0x100) {
                    fprintf(stderr, "Expected %s, got %s\n", expected, core->disasm_str_instr);
                    free(core);
                    return 1;
                }
            }
    for (unsigned space = 0; space < 2; space++)
        for (unsigned write = 0; write < 2; write++)
            for (unsigned address = 0; address < 64; address++) {
                char expected[64];
                core->pc = 0x100;
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x044000 | (write << 15) | (DSP_REG_X0 << 8) |
                    (space ? 0x20 : 0x80) | ((address & 32) << 1) | (address & 31));
                if (write) sprintf(expected, "movep x0,%c:$%06x", space ? 'y' : 'x', 0xffff80+address);
                else sprintf(expected, "movep %c:$%06x,x0", space ? 'y' : 'x', 0xffff80+address);
                strcpy(core->disasm_str_instr, "stale");
                if (disasm_instruction(core, DSP_DISASM_MODE) != 1 || strcmp(core->disasm_str_instr, expected)) {
                    fprintf(stderr, "Expected %s, got %s\n", expected, core->disasm_str_instr);
                    free(core); return 1;
                }
                if (space) {
                    core->registers[DSP_REG_X0] = 0x123456;
                    dsp56k_execute_instruction(core);
                    if (core->pc != 0x101 || core->interrupt_counter || core->interrupt_pipeline_count ||
                        core->registers[DSP_REG_X0] != (write ? 0x123456u : 0xffffffu)) {
                        fprintf(stderr, "Y MOVEP execution failed at address %u\n", address);
                        free(core); return 1;
                    }
                }
            }
    for (unsigned easpace = 0; easpace < 2; easpace++)
        for (unsigned write = 0; write < 2; write++)
            for (unsigned address = 0; address < 64; address++) {
                dsp56k_reset_cpu(core);
                core->pc = 0x100;
                core->registers[DSP_REG_R0] = 0x80;
                dsp56k_write_memory(core, easpace, 0x80, 0x123456);
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x072080 | (write << 15) | (easpace << 6) | address);
                strcpy(core->disasm_str_instr, "stale");
                if (disasm_instruction(core, DSP_DISASM_MODE) != 1 ||
                    !strstr(core->disasm_str_instr, "y:$ffff")) {
                    fprintf(stderr, "Missing Y memory MOVEP formatter: %s\n", core->disasm_str_instr);
                    free(core); return 1;
                }
                dsp56k_execute_instruction(core);
                if (core->pc != 0x101 || core->interrupt_counter || core->interrupt_pipeline_count ||
                    dsp56k_read_memory(core, easpace, 0x80) != (write ? 0x123456u : 0xffffffu) ||
                    core->registers[DSP_REG_R0] != 0x80) {
                    fprintf(stderr, "Y memory MOVEP execution failed\n");
                    free(core); return 1;
                }
            }
    for (unsigned cc = 0; cc < 16; cc++)
        for (unsigned ccr = 0; ccr < 256; ccr++) {
            dsp56k_reset_cpu(core);
            core->registers[DSP_REG_SR] = ccr;
            core->pc = 0x100;
            bool taken = emu_calc_cc(core, cc) != 0;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x300 | cc);
            dsp56k_execute_instruction(core);
            if (core->pc != 0x101 || core->is_debugging != taken ||
                core->is_idle != taken || core->registers[DSP_REG_SR] != ccr ||
                core->interrupt_counter || core->interrupt_pipeline_count) return 1;
            if (taken) {
                dsp56k_execute_instruction(core);
                if (core->pc != 0x101 || core->instr_cycle) return 1;
            }
        }
    dsp56k_reset_cpu(core);
    core->pc = 0x100;
    dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x200);
    dsp56k_execute_instruction(core);
    if (!core->is_debugging || !core->is_idle || core->pc != 0x101 || core->interrupt_counter) return 1;
    dsp56k_reset_cpu(core);
    if (core->is_debugging) return 1;
    const unsigned word_regs[] = {DSP_REG_X0, DSP_REG_X1, DSP_REG_Y0, DSP_REG_Y1,
                                  DSP_REG_A0, DSP_REG_A1, DSP_REG_B0, DSP_REG_B1};
    const uint32_t bus_values[] = {0, 0x123456, 0xabcdef, 0xffffff};
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned r = 0; r < 8; r++)
            for (unsigned v = 0; v < 4; v++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = sixteen << DSP_SR_SA;
                core->read_peripheral = move_bus_read;
                core->write_peripheral = move_bus_write;
                move_bus_value = bus_values[v];
                core->pc = 0x100;
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x084000 | (word_regs[r] << 8));
                dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x08c000 | (word_regs[r] << 8));
                dsp56k_execute_instruction(core);
                uint32_t expected = sixteen ? (bus_values[v] & 0xffff) << 8 : bus_values[v];
                if (core->registers[word_regs[r]] != expected) return 1;
                dsp56k_execute_instruction(core);
                if (move_bus_value != (sixteen ? bus_values[v] & 0xffff : bus_values[v])) return 1;
            }
    const uint32_t accumulator_inputs[] = {0, 0x7fff, 0x8000, 0x123456, 0xffffff};
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned accumulator = 0; accumulator < 2; accumulator++)
            for (unsigned i = 0; i < 5; i++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = sixteen << DSP_SR_SA;
                core->read_peripheral = move_bus_read;
                move_bus_value = accumulator_inputs[i];
                core->pc = 0x100;
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x084000 | ((DSP_REG_A+accumulator) << 8));
                dsp56k_execute_instruction(core);
                uint32_t middle = sixteen ? (move_bus_value & 0xffff) << 8 : move_bus_value;
                if (core->registers[DSP_REG_A0+accumulator] ||
                    core->registers[DSP_REG_A1+accumulator] != middle ||
                    core->registers[DSP_REG_A2+accumulator] != ((middle & 0x800000) ? 0xffu : 0u)) return 1;
            }
    const int64_t read_inputs[] = {0, 1, -1, 0x7fff0000LL, 0x7fffffffLL,
                                  -0x80000000LL, -0x80000001LL, 0x80000000LL,
                                  0x100000000LL, -0x100000000LL};
    for (unsigned accumulator = 0; accumulator < 2; accumulator++)
        for (unsigned scale = 0; scale < 3; scale++)
            for (unsigned i = 0; i < 10; i++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = (1u << DSP_SR_SA) | (scale << DSP_SR_S0);
                int64_t input = read_inputs[i];
                uint64_t packed = (uint64_t)input & ((1ULL << 40)-1);
                core->registers[DSP_REG_A2+accumulator] = packed >> 32;
                core->registers[DSP_REG_A1+accumulator] = ((packed >> 16) & 0xffff) << 8;
                core->registers[DSP_REG_A0+accumulator] = (packed & 0xffff) << 8;
                int64_t scaled = scale == 1 ? input/2 - (input < 0 && input%2 != 0) : scale == 2 ? input*2 : input;
                int64_t word = scaled/65536 - (scaled < 0 && scaled%65536 != 0);
                bool limited = word > 32767 || word < -32768;
                if (word > 32767) word = 32767;
                if (word < -32768) word = -32768;
                core->write_peripheral = move_bus_write;
                core->pc = 0x100;
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x08c000 | ((DSP_REG_A+accumulator) << 8));
                dsp56k_execute_instruction(core);
                if (move_bus_value != ((uint32_t)word & 0xffffff) ||
                    !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_L)) != limited) return 1;
            }
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned reg = DSP_REG_X0; reg <= DSP_REG_B; reg++)
            for (unsigned immediate = 0; immediate < 256; immediate++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = sixteen << DSP_SR_SA;
                core->pc = 0x100;
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x200000 | (reg << 16) | (immediate << 8));
                dsp56k_execute_instruction(core);
                bool fraction = reg <= DSP_REG_Y1 || reg == DSP_REG_A || reg == DSP_REG_B;
                bool partial = reg == DSP_REG_A0 || reg == DSP_REG_A1 || reg == DSP_REG_B0 || reg == DSP_REG_B1;
                unsigned shift = fraction ? 16 : (sixteen && partial) ? 8 : 0;
                uint32_t expected = immediate << shift;
                if (reg == DSP_REG_A || reg == DSP_REG_B) {
                    unsigned which = reg & 1;
                    if (core->registers[DSP_REG_A1+which] != expected || core->registers[DSP_REG_A0+which] ||
                        core->registers[DSP_REG_A2+which] != ((immediate & 128) ? 0xffu : 0u)) return 1;
                } else if (core->registers[reg] != expected) return 1;
                if (core->pc != 0x101 || core->registers[DSP_REG_SR] != (sixteen << DSP_SR_SA)) return 1;
            }
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned accumulator = 0; accumulator < 2; accumulator++)
            for (unsigned value = 0; value < 256; value++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = sixteen << DSP_SR_SA;
                core->registers[DSP_REG_A2+accumulator] = value;
                core->write_peripheral = move_bus_write;
                core->pc = 0x100;
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x08c000 | ((DSP_REG_A2+accumulator) << 8));
                dsp56k_execute_instruction(core);
                uint32_t expected = sixteen && (value & 128) ? value | 0xffff00 : value;
                if (move_bus_value != expected || core->pc != 0x101 ||
                    core->registers[DSP_REG_SR] != (sixteen << DSP_SR_SA)) return 1;
            }
    const unsigned long_regs[][2] = {{DSP_REG_A1,DSP_REG_A0}, {DSP_REG_B1,DSP_REG_B0},
                                            {DSP_REG_X1,DSP_REG_X0}, {DSP_REG_Y1,DSP_REG_Y0}};
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned pair = 0; pair < 4; pair++)
            for (unsigned v = 0; v < 4; v++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = sixteen << DSP_SR_SA;
                core->registers[DSP_REG_A2] = 0x55;
                core->registers[DSP_REG_B2] = 0xaa;
                core->pc = 0x100;
                uint32_t x = bus_values[v], y = bus_values[3-v];
                dsp56k_write_memory(core, DSP_SPACE_X, 0x20, x);
                dsp56k_write_memory(core, DSP_SPACE_Y, 0x20, y);
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x408000 | (pair << 16) | (0x20 << 8));
                dsp56k_write_memory(core, DSP_SPACE_P, 0x101, 0x400000 | (pair << 16) | (0x21 << 8));
                dsp56k_execute_instruction(core);
                if (core->registers[long_regs[pair][0]] != (sixteen ? (x & 0xffff) << 8 : x) ||
                    core->registers[long_regs[pair][1]] != (sixteen ? (y & 0xffff) << 8 : y)) return 1;
                dsp56k_execute_instruction(core);
                if (dsp56k_read_memory(core,DSP_SPACE_X,0x21) != (sixteen ? x & 0xffff : x) ||
                    dsp56k_read_memory(core,DSP_SPACE_Y,0x21) != (sixteen ? y & 0xffff : y) ||
                    core->registers[DSP_REG_A2] != 0x55 || core->registers[DSP_REG_B2] != 0xaa ||
                    core->pc != 0x102 || core->registers[DSP_REG_SR] != (sixteen << DSP_SR_SA)) return 1;
            }
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned pair = 4; pair < 8; pair++)
            for (unsigned v = 0; v < 4; v++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = sixteen << DSP_SR_SA;
                core->pc = 0x100;
                uint32_t x = bus_values[v], y = bus_values[3-v];
                dsp56k_write_memory(core, DSP_SPACE_X, 0x20, x);
                dsp56k_write_memory(core, DSP_SPACE_Y, 0x20, y);
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                    0x408000 | ((pair & 3) << 16) | ((pair & 4) << 17) | (0x20 << 8));
                dsp56k_execute_instruction(core);
                unsigned first = pair == 5 || pair == 7;
                uint32_t high = sixteen ? (x & 0xffff) << 8 : x;
                uint32_t low = pair < 6 ? (sixteen ? (y & 0xffff) << 8 : y) : 0;
                if (core->registers[DSP_REG_A1+first] != high ||
                    core->registers[DSP_REG_A0+first] != low ||
                    core->registers[DSP_REG_A2+first] != ((high & 0x800000) ? 0xffu : 0u)) return 1;
                if (pair >= 6) {
                    high = sixteen ? (y & 0xffff) << 8 : y;
                    if (core->registers[DSP_REG_A1+(first^1)] != high ||
                        core->registers[DSP_REG_A0+(first^1)] ||
                        core->registers[DSP_REG_A2+(first^1)] != ((high & 0x800000) ? 0xffu : 0u)) return 1;
                }
                if (core->pc != 0x101 || core->registers[DSP_REG_SR] != (sixteen << DSP_SR_SA)) return 1;
            }
    for (unsigned accumulator = 0; accumulator < 2; accumulator++)
        for (unsigned scale = 0; scale < 3; scale++)
            for (unsigned i = 0; i < 10; i++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = (1u << DSP_SR_SA) | (scale << DSP_SR_S0);
                uint64_t packed = (uint64_t)read_inputs[i] & ((1ULL << 40)-1);
                core->registers[DSP_REG_A2+accumulator] = packed >> 32;
                core->registers[DSP_REG_A1+accumulator] = ((packed >> 16) & 0xffff) << 8;
                core->registers[DSP_REG_A0+accumulator] = (packed & 0xffff) << 8;
                int64_t expected = read_inputs[i];
                if (scale == 1) expected = expected/2 - (expected < 0 && expected%2 != 0);
                else if (scale == 2) expected *= 2;
                bool limited = expected > 0x7fffffffLL || expected < -0x80000000LL;
                if (expected > 0x7fffffffLL) expected = 0x7fffffffLL;
                if (expected < -0x80000000LL) expected = -0x80000000LL;
                core->pc = 0x100;
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x480000 | (accumulator << 16) | (0x20 << 8));
                dsp56k_execute_instruction(core);
                if (dsp56k_read_memory(core,DSP_SPACE_X,0x20) != (((uint64_t)expected >> 16) & 0xffffff) ||
                    dsp56k_read_memory(core,DSP_SPACE_Y,0x20) != ((uint32_t)expected & 0xffff) ||
                    !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_L)) != limited || core->pc != 0x101) return 1;
            }
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned reg = DSP_REG_X0; reg <= DSP_REG_B; reg++)
            for (unsigned v = 0; v < 4; v++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = sixteen << DSP_SR_SA;
                core->registers[DSP_REG_R0] = bus_values[v];
                core->pc = 0x100;
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                    0x200000 | (DSP_REG_R0 << 13) | (reg << 8));
                dsp56k_execute_instruction(core);
                uint32_t expected = bus_values[v];
                if (sixteen && reg != DSP_REG_A2 && reg != DSP_REG_B2) expected = (expected & 0xffff) << 8;
                if (reg == DSP_REG_A || reg == DSP_REG_B) {
                    unsigned which = reg & 1;
                    if (core->registers[DSP_REG_A1+which] != expected || core->registers[DSP_REG_A0+which] ||
                        core->registers[DSP_REG_A2+which] != ((expected & 0x800000) ? 0xffu : 0u)) return 1;
                } else {
                    if (reg == DSP_REG_A2 || reg == DSP_REG_B2) expected &= 0xff;
                    if (core->registers[reg] != expected) return 1;
                }
                if (core->pc != 0x101 || core->registers[DSP_REG_SR] != (sixteen << DSP_SR_SA)) return 1;
            }
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned accumulator = 0; accumulator < 2; accumulator++)
            for (unsigned value = 0; value < 256; value++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = sixteen << DSP_SR_SA;
                core->registers[DSP_REG_A1+accumulator] = value;
                core->registers[DSP_REG_A0+accumulator] = 255-value;
                core->pc = 0x100;
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x200003 | (accumulator << 3));
                dsp56k_execute_instruction(core);
                if (!!(core->registers[DSP_REG_SR] & (1u << DSP_SR_Z)) != sixteen ||
                    core->registers[DSP_REG_A1+accumulator] != value ||
                    core->registers[DSP_REG_A0+accumulator] != 255-value || core->pc != 0x101) return 1;
            }
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned op = 0; op < 8; op++)
            for (unsigned carry = 0; carry < 2; carry++) {
                dsp56k_reset_cpu(core);
                unsigned accumulator = op & 1;
                unsigned subtract = (op >> 1) & 1;
                unsigned y = op >> 2;
                core->registers[DSP_REG_SR] = (sixteen << DSP_SR_SA) | carry;
                core->registers[DSP_REG_A0+accumulator] = 0x10000;
                core->pc = 0x100;
                unsigned alu = (y ? 0x31 : 0x21) | (accumulator << 3) | (subtract << 2);
                dsp56k_write_memory(core, DSP_SPACE_P, 0x100, 0x200000 | alu);
                dsp56k_execute_instruction(core);
                uint32_t delta = carry << (sixteen ? 8 : 0);
                if (core->registers[DSP_REG_A0+accumulator] != (subtract ? 0x10000-delta : 0x10000+delta) ||
                    core->registers[DSP_REG_A1+accumulator] || core->registers[DSP_REG_A2+accumulator] ||
                    core->pc != 0x101) return 1;
            }
    const uint64_t carry_inputs[] = {0, 0xffff, 0xffffffff, 0x7fffffffffULL, 0x8000000000ULL, 0xffffffffffULL};
    for (unsigned op = 0; op < 8; op++)
        for (unsigned i = 0; i < 6; i++) {
            dsp56k_reset_cpu(core);
            unsigned accumulator = op & 1, subtract = (op >> 1) & 1, y = op >> 2;
            uint64_t input = carry_inputs[i];
            core->registers[DSP_REG_SR] = (1u << DSP_SR_SA) | 1;
            core->registers[DSP_REG_A2+accumulator] = input >> 32;
            core->registers[DSP_REG_A1+accumulator] = ((input >> 16) & 0xffff) << 8;
            core->registers[DSP_REG_A0+accumulator] = (input & 0xffff) << 8;
            core->pc = 0x100;
            dsp56k_write_memory(core, DSP_SPACE_P, 0x100,
                0x200000 | (y ? 0x31 : 0x21) | (accumulator << 3) | (subtract << 2));
            dsp56k_execute_instruction(core);
            uint64_t expected = (subtract ? input-1 : input+1) & 0xffffffffffULL;
            if (core->registers[DSP_REG_A2+accumulator] != expected >> 32 ||
                core->registers[DSP_REG_A1+accumulator] != ((expected >> 16) & 0xffff) << 8 ||
                core->registers[DSP_REG_A0+accumulator] != (expected & 0xffff) << 8) return 1;
        }
    const int64_t carry_sources[] = {0, 1, -1, 0x7fffffff, -0x80000000LL};
    for (unsigned with_carry = 0; with_carry < 2; with_carry++)
    for (unsigned op = 0; op < 8; op++)
        for (unsigned i = 0; i < 6; i++)
            for (unsigned j = 0; j < 5; j++)
                for (unsigned carry = 0; carry < 2; carry++) {
                    dsp56k_reset_cpu(core);
                    unsigned acc = op & 1, subtract = (op >> 1) & 1, y = op >> 2;
                    unsigned incoming = with_carry ? carry : 0;
                    uint64_t input = carry_inputs[i], src = (uint64_t)carry_sources[j] & 0xffffffffffULL;
                    core->registers[DSP_REG_SR] = (1u << DSP_SR_SA) | carry;
                    core->registers[DSP_REG_A2+acc] = input >> 32;
                    core->registers[DSP_REG_A1+acc] = (((input >> 16) & 0xffff) << 8) | 0x55;
                    core->registers[DSP_REG_A0+acc] = ((input & 0xffff) << 8) | 0xaa;
                    core->registers[(y ? DSP_REG_Y1 : DSP_REG_X1)] = (((src >> 16) & 0xffff) << 8) | 0x33;
                    core->registers[(y ? DSP_REG_Y0 : DSP_REG_X0)] = ((src & 0xffff) << 8) | 0x77;
                    core->pc = 0x100;
                    dsp56k_write_memory(core,DSP_SPACE_P,0x100,0x200000 | (y ? 0x30 : 0x20) | with_carry | (acc << 3) | (subtract << 2));
                    dsp56k_execute_instruction(core);
                    int64_t signed_input = (int64_t)input - ((input & (1ULL << 39)) ? 1LL << 40 : 0);
                    int64_t signed_result = subtract ? signed_input-carry_sources[j]-incoming : signed_input+carry_sources[j]+incoming;
                    uint64_t result = (uint64_t)signed_result & 0xffffffffffULL;
                    bool overflow = signed_result < -(1LL << 39) || signed_result >= (1LL << 39);
                    bool carry_out = subtract ? input < src+incoming : input+src+incoming > 0xffffffffffULL;
                    if (core->registers[DSP_REG_A2+acc] != result >> 32 ||
                        core->registers[DSP_REG_A1+acc] != ((result >> 16) & 0xffff) << 8 ||
                        core->registers[DSP_REG_A0+acc] != (result & 0xffff) << 8 ||
                        !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_C)) != carry_out ||
                        !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_V)) != overflow) return 1;
                }
    for (unsigned op = 0; op < 4; op++)
        for (unsigned i = 0; i < 6; i++)
            for (unsigned j = 0; j < 6; j++) {
                dsp56k_reset_cpu(core);
                unsigned acc = op & 1, subtract = op >> 1;
                uint64_t dst = carry_inputs[i], src = carry_inputs[j];
                core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
                for (unsigned which = 0; which < 2; which++) {
                    uint64_t value = which == acc ? dst : src;
                    core->registers[DSP_REG_A2+which] = value >> 32;
                    core->registers[DSP_REG_A1+which] = (((value >> 16) & 0xffff) << 8) | 0x55;
                    core->registers[DSP_REG_A0+which] = ((value & 0xffff) << 8) | 0xaa;
                }
                core->pc = 0x100;
                dsp56k_write_memory(core,DSP_SPACE_P,0x100,0x200010 | (acc << 3) | (subtract << 2));
                dsp56k_execute_instruction(core);
                int64_t sd = (int64_t)dst - ((dst & (1ULL << 39)) ? 1LL << 40 : 0);
                int64_t ss = (int64_t)src - ((src & (1ULL << 39)) ? 1LL << 40 : 0);
                int64_t sr = subtract ? sd-ss : sd+ss;
                uint64_t result = (uint64_t)sr & 0xffffffffffULL;
                bool overflow = sr < -(1LL << 39) || sr >= (1LL << 39);
                bool carry = subtract ? dst < src : dst+src > 0xffffffffffULL;
                if (core->registers[DSP_REG_A2+acc] != result >> 32 ||
                    core->registers[DSP_REG_A1+acc] != ((result >> 16) & 0xffff) << 8 ||
                    core->registers[DSP_REG_A0+acc] != (result & 0xffff) << 8 ||
                    !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_C)) != carry ||
                    !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_V)) != overflow) return 1;
            }
    const int32_t word_sources[] = {0, 1, -1, 32767, -32768};
    const unsigned source_regs[] = {DSP_REG_X0,DSP_REG_Y0,DSP_REG_X1,DSP_REG_Y1};
    for (unsigned compare = 0; compare < 2; compare++)
    for (unsigned op = 0; op < 16; op++)
        for (unsigned i = 0; i < 6; i++)
            for (unsigned j = 0; j < 5; j++) {
                dsp56k_reset_cpu(core);
                unsigned acc = op & 1, subtract = (op >> 1) & 1, source = op >> 2;
                if (compare && !subtract) continue;
                uint64_t dst = carry_inputs[i];
                int64_t ss = (int64_t)word_sources[j]*65536;
                uint64_t src = (uint64_t)ss & 0xffffffffffULL;
                core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
                core->registers[DSP_REG_A2+acc] = dst >> 32;
                core->registers[DSP_REG_A1+acc] = (((dst >> 16) & 0xffff) << 8) | 0x55;
                core->registers[DSP_REG_A0+acc] = ((dst & 0xffff) << 8) | 0xaa;
                core->registers[source_regs[source]] = (((uint32_t)word_sources[j] & 0xffff) << 8) | 0x77;
                uint32_t saved[6];
                memcpy(saved, &core->registers[DSP_REG_A0], sizeof(saved));
                core->pc = 0x100;
                dsp56k_write_memory(core,DSP_SPACE_P,0x100,0x200040 | (source << 4) | (acc << 3) | (subtract << 2) | compare);
                dsp56k_execute_instruction(core);
                int64_t sd = (int64_t)dst - ((dst & (1ULL << 39)) ? 1LL << 40 : 0);
                int64_t sr = subtract ? sd-ss : sd+ss;
                uint64_t result = (uint64_t)sr & 0xffffffffffULL;
                bool overflow = sr < -(1LL << 39) || sr >= (1LL << 39);
                bool carry = subtract ? dst < src : dst+src > 0xffffffffffULL;
                if ((!compare && (core->registers[DSP_REG_A2+acc] != result >> 32 ||
                    core->registers[DSP_REG_A1+acc] != ((result >> 16) & 0xffff) << 8 ||
                    core->registers[DSP_REG_A0+acc] != (result & 0xffff) << 8)) ||
                    (compare && memcmp(saved, &core->registers[DSP_REG_A0], sizeof(saved))) ||
                    !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_C)) != carry ||
                    !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_V)) != overflow) return 1;
            }
    for (unsigned acc = 0; acc < 2; acc++)
        for (unsigned i = 0; i < 6; i++)
            for (unsigned j = 0; j < 6; j++) {
                dsp56k_reset_cpu(core);
                uint64_t dst = carry_inputs[i], src = carry_inputs[j];
                core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
                for (unsigned which = 0; which < 2; which++) {
                    uint64_t value = which == acc ? dst : src;
                    core->registers[DSP_REG_A2+which] = value >> 32;
                    core->registers[DSP_REG_A1+which] = (((value >> 16) & 0xffff) << 8) | (which ? 0x55 : 0x33);
                    core->registers[DSP_REG_A0+which] = ((value & 0xffff) << 8) | (which ? 0xaa : 0x77);
                }
                uint32_t saved[6];
                memcpy(saved, &core->registers[DSP_REG_A0], sizeof(saved));
                core->pc = 0x100;
                dsp56k_write_memory(core,DSP_SPACE_P,0x100,0x200005 | (acc << 3));
                dsp56k_execute_instruction(core);
                int64_t sd = (int64_t)dst - ((dst & (1ULL << 39)) ? 1LL << 40 : 0);
                int64_t ss = (int64_t)src - ((src & (1ULL << 39)) ? 1LL << 40 : 0);
                int64_t sr = sd-ss;
                uint64_t result = (uint64_t)sr & 0xffffffffffULL;
                bool overflow = sr < -(1LL << 39) || sr >= (1LL << 39);
                if (memcmp(saved, &core->registers[DSP_REG_A0], sizeof(saved)) ||
                    !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_C)) != (dst < src) ||
                    !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_V)) != overflow ||
                    !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_Z)) != (result == 0) ||
                    !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_N)) != !!(result & (1ULL << 39))) return 1;
            }
    const uint32_t not_inputs[] = {0, 0xff, 0xffff00, 0xffffff, 0x7fff55, 0x8000aa};
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned acc = 0; acc < 2; acc++)
            for (unsigned i = 0; i < 6; i++) {
                dsp56k_reset_cpu(core);
                core->registers[DSP_REG_SR] = (sixteen << DSP_SR_SA) | (1u << DSP_SR_C) | (1u << DSP_SR_L);
                core->registers[DSP_REG_A1+acc] = not_inputs[i];
                core->registers[DSP_REG_A0+acc] = 0x123456;
                core->registers[DSP_REG_A2+acc] = 0x55;
                core->pc = 0x100;
                dsp56k_write_memory(core,DSP_SPACE_P,0x100,0x200017 | (acc << 3));
                dsp56k_execute_instruction(core);
                uint32_t result = ~not_inputs[i] & (sixteen ? 0xffff00 : 0xffffff);
                uint32_t expected_sr = (sixteen << DSP_SR_SA) | (1u << DSP_SR_C) | (1u << DSP_SR_L) |
                    ((result == 0) << DSP_SR_Z) | (((result >> 23) & 1) << DSP_SR_N);
                if (core->registers[DSP_REG_A1+acc] != result || core->registers[DSP_REG_A0+acc] != 0x123456 ||
                    core->registers[DSP_REG_A2+acc] != 0x55 || core->registers[DSP_REG_SR] != expected_sr) return 1;
            }
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned source = 0; source < 4; source++)
            for (unsigned acc = 0; acc < 2; acc++)
                for (unsigned operation = 0; operation < 3; operation++)
                    for (unsigned i = 0; i < 6; i++)
                        for (unsigned j = 0; j < 6; j++) {
                            dsp56k_reset_cpu(core);
                            core->registers[DSP_REG_SR] = (sixteen << DSP_SR_SA) | (1u << DSP_SR_C) | (1u << DSP_SR_L);
                            core->registers[DSP_REG_A1+acc] = not_inputs[i];
                            core->registers[DSP_REG_A0+acc] = 0x123456;
                            core->registers[DSP_REG_A2+acc] = 0x55;
                            core->registers[source_regs[source]] = not_inputs[j];
                            core->pc = 0x100;
                            unsigned opcode = operation == 0 ? 2 : operation == 1 ? 3 : 6;
                            dsp56k_write_memory(core,DSP_SPACE_P,0x100,0x200040 | (source << 4) | (acc << 3) | opcode);
                            dsp56k_execute_instruction(core);
                            uint32_t result = operation == 0 ? not_inputs[i] | not_inputs[j] :
                                              operation == 1 ? not_inputs[i] ^ not_inputs[j] : not_inputs[i] & not_inputs[j];
                            result &= sixteen ? 0xffff00 : 0xffffff;
                            uint32_t expected_sr = (sixteen << DSP_SR_SA) | (1u << DSP_SR_C) | (1u << DSP_SR_L) |
                                ((result == 0) << DSP_SR_Z) | (((result >> 23) & 1) << DSP_SR_N);
                            if (core->registers[DSP_REG_A1+acc] != result || core->registers[DSP_REG_A0+acc] != 0x123456 ||
                                core->registers[DSP_REG_A2+acc] != 0x55 || core->registers[DSP_REG_SR] != expected_sr) return 1;
                        }
    const uint32_t shift_inputs[] = {0, 1, 0xff, 0x100, 0xffff00, 0xffffff, 0x7fff55, 0x8000aa};
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned acc = 0; acc < 2; acc++)
            for (unsigned left = 0; left < 2; left++)
                for (unsigned i = 0; i < 8; i++) {
                    dsp56k_reset_cpu(core);
                    core->registers[DSP_REG_SR] = (sixteen << DSP_SR_SA) | (1u << DSP_SR_L);
                    core->registers[DSP_REG_A1+acc] = shift_inputs[i];
                    core->registers[DSP_REG_A0+acc] = 0x123456;
                    core->registers[DSP_REG_A2+acc] = 0x55;
                    core->pc = 0x100;
                    dsp56k_write_memory(core,DSP_SPACE_P,0x100,0x200023 | (acc << 3) | (left << 4));
                    dsp56k_execute_instruction(core);
                    unsigned width = sixteen ? 16 : 24, padding = sixteen ? 8 : 0;
                    uint32_t word = shift_inputs[i] >> padding;
                    unsigned carry = left ? (word >> (width-1)) & 1 : word & 1;
                    uint32_t result = (left ? word << 1 : word >> 1) & ((1u << width)-1);
                    result <<= padding;
                    uint32_t expected_sr = (sixteen << DSP_SR_SA) | (1u << DSP_SR_L) | carry |
                        ((result == 0) << DSP_SR_Z) | (((result >> 23) & 1) << DSP_SR_N);
                    if (core->registers[DSP_REG_A1+acc] != result || core->registers[DSP_REG_A0+acc] != 0x123456 ||
                        core->registers[DSP_REG_A2+acc] != 0x55 || core->registers[DSP_REG_SR] != expected_sr) return 1;
                }
    for (unsigned incoming = 0; incoming < 2; incoming++)
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned acc = 0; acc < 2; acc++)
            for (unsigned left = 0; left < 2; left++)
                for (unsigned i = 0; i < 8; i++) {
                    dsp56k_reset_cpu(core);
                    core->registers[DSP_REG_SR] = (sixteen << DSP_SR_SA) | (1u << DSP_SR_L) | incoming;
                    core->registers[DSP_REG_A1+acc] = shift_inputs[i];
                    core->registers[DSP_REG_A0+acc] = 0x123456;
                    core->registers[DSP_REG_A2+acc] = 0x55;
                    core->pc = 0x100;
                    dsp56k_write_memory(core,DSP_SPACE_P,0x100,0x200027 | (acc << 3) | (left << 4));
                    dsp56k_execute_instruction(core);
                    unsigned width = sixteen ? 16 : 24, padding = sixteen ? 8 : 0;
                    uint32_t word = shift_inputs[i] >> padding;
                    unsigned carry = left ? (word >> (width-1)) & 1 : word & 1;
                    uint32_t result = (left ? (word << 1) | incoming : (word >> 1) | (incoming << (width-1))) & ((1u << width)-1);
                    result <<= padding;
                    uint32_t expected_sr = (sixteen << DSP_SR_SA) | (1u << DSP_SR_L) | carry |
                        ((result == 0) << DSP_SR_Z) | (((result >> 23) & 1) << DSP_SR_N);
                    if (core->registers[DSP_REG_A1+acc] != result || core->registers[DSP_REG_A0+acc] != 0x123456 ||
                        core->registers[DSP_REG_A2+acc] != 0x55 || core->registers[DSP_REG_SR] != expected_sr) return 1;
                }
    for (unsigned sixteen = 0; sixteen < 2; sixteen++)
        for (unsigned acc = 0; acc < 2; acc++)
            for (unsigned right = 0; right < 2; right++)
                for (unsigned count = 0; count < 32; count++)
                    for (unsigned i = 0; i < 8; i++) {
                        dsp56k_reset_cpu(core);
                        core->registers[DSP_REG_SR] = (sixteen << DSP_SR_SA) | (1u << DSP_SR_L);
                        core->registers[DSP_REG_A1+acc] = shift_inputs[i];
                        core->registers[DSP_REG_A0+acc] = 0x123456;
                        core->registers[DSP_REG_A2+acc] = 0x55;
                        core->pc = 0x100;
                        dsp56k_write_memory(core,DSP_SPACE_P,0x100,0x0c1e80 | (right << 6) | (count << 1) | acc);
                        dsp56k_execute_instruction(core);
                        unsigned width = sixteen ? 16 : 24, padding = sixteen ? 8 : 0, carry = 0;
                        uint32_t word = shift_inputs[i] >> padding, mask = (1u << width)-1;
                        for (unsigned step = 0; step < count; step++) {
                            carry = right ? word & 1 : (word >> (width-1)) & 1;
                            word = (right ? word >> 1 : word << 1) & mask;
                        }
                        uint32_t result = word << padding;
                        uint32_t expected_sr = (sixteen << DSP_SR_SA) | (1u << DSP_SR_L) | carry |
                            ((result == 0) << DSP_SR_Z) | (((result >> 23) & 1) << DSP_SR_N);
                        if (core->registers[DSP_REG_A1+acc] != result || core->registers[DSP_REG_A0+acc] != 0x123456 ||
                            core->registers[DSP_REG_A2+acc] != 0x55 || core->registers[DSP_REG_SR] != expected_sr || core->pc != 0x101) return 1;
                    }
    const unsigned count_regs[] = {DSP_REG_A1,DSP_REG_B1,DSP_REG_X0,DSP_REG_Y0,DSP_REG_X1,DSP_REG_Y1};
    for (unsigned source = 0; source < 6; source++)
        for (unsigned acc = 0; acc < 2; acc++)
            for (unsigned right = 0; right < 2; right++)
                for (unsigned count = 0; count <= 24; count++)
                    for (unsigned i = 0; i < 8; i++) {
                        dsp56k_reset_cpu(core);
                        core->registers[DSP_REG_SR] = 1u << DSP_SR_L;
                        core->registers[DSP_REG_A1+acc] = shift_inputs[i];
                        core->registers[count_regs[source]] = (shift_inputs[i] & ~31u) | count;
                        uint32_t word = core->registers[DSP_REG_A1+acc], carry = 0;
                        core->pc = 0x100;
                        dsp56k_write_memory(core,DSP_SPACE_P,0x100,0x0c1e10 | (right << 5) | ((source+2) << 1) | acc);
                        dsp56k_execute_instruction(core);
                        for (unsigned step = 0; step < count; step++) {
                            carry = right ? word & 1 : (word >> 23) & 1;
                            word = (right ? word >> 1 : word << 1) & 0xffffff;
                        }
                        uint32_t expected_sr = (1u << DSP_SR_L) | carry |
                            ((word == 0) << DSP_SR_Z) | (((word >> 23) & 1) << DSP_SR_N);
                        if (core->registers[DSP_REG_A1+acc] != word || core->registers[DSP_REG_SR] != expected_sr || core->pc != 0x101) return 1;
                    }
    for (unsigned acc = 0; acc < 2; acc++)
        for (unsigned left = 0; left < 2; left++)
            for (unsigned i = 0; i < 6; i++) {
                dsp56k_reset_cpu(core);
                uint64_t input = carry_inputs[i];
                core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
                core->registers[DSP_REG_A2+acc] = input >> 32;
                core->registers[DSP_REG_A1+acc] = (((input >> 16) & 0xffff) << 8) | 0x55;
                core->registers[DSP_REG_A0+acc] = ((input & 0xffff) << 8) | 0xaa;
                core->pc = 0x100;
                dsp56k_write_memory(core,DSP_SPACE_P,0x100,0x200022 | (acc << 3) | (left << 4));
                dsp56k_execute_instruction(core);
                int64_t signed_input = (int64_t)input - ((input & (1ULL << 39)) ? 1LL << 40 : 0);
                int64_t shifted = left ? signed_input*2 : signed_input/2 - (signed_input < 0 && signed_input%2 != 0);
                uint64_t result = (uint64_t)shifted & 0xffffffffffULL;
                bool overflow = left && (shifted < -(1LL << 39) || shifted >= (1LL << 39));
                unsigned carry = left ? (input >> 39) & 1 : input & 1;
                if (core->registers[DSP_REG_A2+acc] != result >> 32 ||
                    core->registers[DSP_REG_A1+acc] != ((result >> 16) & 0xffff) << 8 ||
                    core->registers[DSP_REG_A0+acc] != (result & 0xffff) << 8 ||
                    !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_C)) != carry ||
                    !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_V)) != overflow) return 1;
            }
    for (unsigned source = 0; source < 2; source++)
        for (unsigned acc = 0; acc < 2; acc++)
            for (unsigned left = 0; left < 2; left++)
                for (unsigned count = 0; count < 40; count++)
                    for (unsigned i = 0; i < 6; i++) {
                        dsp56k_reset_cpu(core);
                        uint64_t word = carry_inputs[i];
                        core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
                        core->registers[DSP_REG_A2+source] = word >> 32;
                        core->registers[DSP_REG_A1+source] = (((word >> 16) & 0xffff) << 8) | 0x55;
                        core->registers[DSP_REG_A0+source] = ((word & 0xffff) << 8) | 0xaa;
                        core->pc = 0x100;
                        dsp56k_write_memory(core,DSP_SPACE_P,0x100,0x0c1c00 | (left << 8) | (source << 7) | (count << 1) | acc);
                        dsp56k_execute_instruction(core);
                        unsigned carry = 0, overflow = 0;
                        for (unsigned step = 0; step < count; step++) {
                            unsigned sign = (word >> 39) & 1;
                            carry = left ? sign : word & 1;
                            word = (left ? word << 1 : (word >> 1) | ((uint64_t)sign << 39)) & 0xffffffffffULL;
                            overflow |= left && (((word >> 39) & 1) != sign);
                        }
                        if (core->registers[DSP_REG_A2+acc] != word >> 32 ||
                            core->registers[DSP_REG_A1+acc] != ((word >> 16) & 0xffff) << 8 ||
                            core->registers[DSP_REG_A0+acc] != (word & 0xffff) << 8 ||
                            !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_C)) != carry ||
                            !!(core->registers[DSP_REG_SR] & (1u << DSP_SR_V)) != overflow) return 1;
                    }
    const unsigned multiply_regs[][2] = {{DSP_REG_X0,DSP_REG_X0},{DSP_REG_Y0,DSP_REG_Y0},
        {DSP_REG_X1,DSP_REG_X0},{DSP_REG_Y1,DSP_REG_Y0},{DSP_REG_X0,DSP_REG_Y1},
        {DSP_REG_Y0,DSP_REG_X0},{DSP_REG_X1,DSP_REG_Y0},{DSP_REG_Y1,DSP_REG_X1}};
    for (unsigned accumulated = 0; accumulated < 2; accumulated++)
    for (unsigned rounded = 0; rounded < 2; rounded++)
    for (unsigned pair = 0; pair < 8; pair++)
        for (unsigned acc = 0; acc < 2; acc++)
            for (unsigned negative = 0; negative < 2; negative++)
                for (unsigned i = 0; i < 5; i++)
                    for (unsigned j = 0; j < 5; j++) {
                        dsp56k_reset_cpu(core);
                        core->registers[DSP_REG_SR] = 1u << DSP_SR_SA;
                        core->registers[multiply_regs[pair][0]] = (((uint32_t)word_sources[i] & 0xffff) << 8) | 0x55;
                        core->registers[multiply_regs[pair][1]] = (((uint32_t)word_sources[j] & 0xffff) << 8) | 0xaa;
                        int64_t lhs = multiply_regs[pair][0] == multiply_regs[pair][1] ? word_sources[j] : word_sources[i];
                        int64_t result_signed = lhs*word_sources[j]*2;
                        if (negative) result_signed = -result_signed;
                        if (accumulated) {
                            core->registers[DSP_REG_A2+acc] = 0x7f;
                            core->registers[DSP_REG_A1+acc] = 0xffff55;
                            core->registers[DSP_REG_A0+acc] = 0xffffaa;
                            result_signed += 0x7fffffffffLL;
                            uint64_t wrapped = (uint64_t)result_signed & 0xffffffffffULL;
                            result_signed = (int64_t)wrapped - ((wrapped & (1ULL << 39)) ? 1LL << 40 : 0);
                        }
                        if (rounded) {
                            int64_t quotient = result_signed/65536 - (result_signed < 0 && result_signed%65536 != 0);
                            int64_t remainder = result_signed-quotient*65536;
                            if (remainder > 32768 || (remainder == 32768 && quotient%2 != 0)) quotient++;
                            result_signed = quotient*65536;
                        }
                        uint64_t result = (uint64_t)result_signed & 0xffffffffffULL;
                        core->pc = 0x100;
                        dsp56k_write_memory(core,DSP_SPACE_P,0x100,0x200080 | (pair << 4) | (acc << 3) | (negative << 2) | (accumulated << 1) | rounded);
                        dsp56k_execute_instruction(core);
                        if (core->registers[DSP_REG_A2+acc] != result >> 32 ||
                            core->registers[DSP_REG_A1+acc] != ((result >> 16) & 0xffff) << 8 ||
                            core->registers[DSP_REG_A0+acc] != (result & 0xffff) << 8 || core->pc != 0x101) {
                            fprintf(stderr,"multiply acc=%u round=%u pair=%u dest=%u sign=%u i=%u j=%u actual=%02x:%06x:%06x expected=%010llx\n", accumulated,rounded,pair,acc,negative,i,j,core->registers[DSP_REG_A2+acc],core->registers[DSP_REG_A1+acc],core->registers[DSP_REG_A0+acc],(unsigned long long)result);
                            return 1;
                        }
                    }
    if (check_sa_alu_frontier(core)) {
        free(core);
        return 1;
    }
    free(core);
    return 0;
}
