/* Compare the instruction-boundary fast path with the unchanged full handler.
 * Include the production source to exercise its private interrupt pipeline
 * without exposing an extra runtime API or substituting an interrupt model. */
#include "../../src/apu/dsp/interp/dsp_cpu.c"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "Interrupt fast-path check line %d: %s\n", __LINE__, #condition); \
    exit(1); } } while (0)

int main(void)
{
    dsp_core_t *initial = calloc(1, sizeof *initial);
    dsp_core_t *reference = malloc(sizeof *reference);
    dsp_core_t *actual = malloc(sizeof *actual);
    CHECK(initial && reference && actual);
    dsp56k_reset_cpu(initial);
    initial->pc = 0x100;
    initial->interrupt_save_pc = 0x101;
    initial->interrupt_instr_fetch = 8;
    initial->interrupt_ipl_to_raise = 2;
    initial->registers[DSP_REG_LC] = 3;
    initial->registers[DSP_REG_LA] = 0x110;
    /* Long-vector entry exercises stack and SR changes during the pipeline. */
    initial->pram[8] = 0x0d0200;
    initial->pram[9] = 0;

    const unsigned pending_masks[] = {0, 1, 8, 15};
    unsigned cases = 0;
    for (unsigned state = DSP_INTERRUPT_NONE; state <= DSP_INTERRUPT_LONG; ++state)
    for (unsigned pipeline = 0; pipeline <= 6; ++pipeline)
    for (unsigned rep = 0; rep <= 2; ++rep)
    for (unsigned flags = 0; flags < 8; ++flags)
    for (unsigned compatibility = 0; compatibility < 2; ++compatibility)
    for (unsigned mask = 0; mask < ARRAY_SIZE(pending_masks); ++mask) {
        *reference = *initial;
        reference->interrupt_state = (uint16_t)state;
        reference->interrupt_pipeline_count = (uint16_t)pipeline;
        reference->loop_rep = rep;
        reference->is_stopped = (flags & 1) != 0;
        reference->is_debugging = (flags & 2) != 0;
        reference->is_waiting = (flags & 4) != 0;
        reference->registers[DSP_REG_SR] =
            (compatibility << DSP_SR_SC) | ((pipeline & 3) << DSP_SR_I0);
        reference->interrupt_counter = 0;
        for (unsigned i = 0; i < 4; ++i) {
            reference->interrupt_is_pending[i] = (pending_masks[mask] >> i) & 1;
            reference->interrupt_counter += reference->interrupt_is_pending[i];
            reference->interrupt_ipl[i] = (int16_t)i;
        }
        *actual = *reference;
        dsp_postexecute_interrupts(reference);
        dsp_postexecute_interrupts_if_needed(actual);
        CHECK(!memcmp(reference, actual, sizeof *actual));
        ++cases;
    }
    printf("Interrupt fast path exact full-state equivalence passed: %u cases\n", cases);
    free(initial); free(reference); free(actual);
    return 0;
}
