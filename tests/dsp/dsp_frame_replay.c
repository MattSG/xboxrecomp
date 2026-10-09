#include "dsp.h"
#include "dsp_port.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Frame replay line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static FILE *input;
static DSPState *processor;
static unsigned transfers;
static void transfer(unsigned kind, uint8_t *ptr, uint32_t address, size_t len, bool dir)
{
    uint32_t event[5];
    CHECK(fread(event, sizeof(event), 1, input) == 1);
    CHECK(event[0] == kind && event[1] == address && event[2] == len && event[3] == dir);
    CHECK(event[4] <= 1);
    if (event[4]) processor->dma.error = true;
    else {
        uint8_t *payload = malloc(len ? len : 1);
        CHECK(payload && fread(payload, 1, len, input) == len);
        if (dir) CHECK(!memcmp(ptr, payload, len));
        else memcpy(ptr, payload, len);
        free(payload);
    }
    transfers++;
}
static void scratch(void *opaque, uint8_t *ptr, uint32_t address, size_t len, bool dir)
{
    (void)opaque;
    transfer(1, ptr, address, len, dir);
}
static void fifo(void *opaque, uint8_t *ptr, unsigned index, size_t len, bool dir)
{
    (void)opaque;
    transfer(2, ptr, index, len, dir);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    input = fopen(argv[1], "rb");
    CHECK(input);
    uint32_t header[3];
    DSPState initial, expected;
    CHECK(fread(header, sizeof(header), 1, input) == 1);
    if (header[1] != DSP_FRAME_CAPTURE_VERSION)
        fprintf(stderr, "Unsupported DSP capture version %u; regenerate with the current executable\n", header[1]);
    CHECK(header[0] == 0x44535046 && header[1] == DSP_FRAME_CAPTURE_VERSION && header[2] == sizeof(initial));
    CHECK(fread(&initial, sizeof(initial), 1, input) == 1);
    /* Captures serialize state, never executable host cache pointers. */
    CHECK(initial.core.pc < DSP_PRAM_SIZE);
    for (unsigned i = 0; i < DSP_PRAM_SIZE; i++) CHECK(!initial.core.pram_opcache[i]);
    CHECK(dsp_has_work(&initial));
    processor = dsp_init(NULL, scratch, fifo, initial.is_gp);
    CHECK(processor);
    DSPState wiring = *processor;
    *processor = initial;
    processor->core.opaque = processor;
    processor->core.read_peripheral = wiring.core.read_peripheral;
    processor->core.write_peripheral = wiring.core.write_peripheral;
    processor->dma.mem_opaque = &processor->core;
    processor->dma.mem_read = wiring.dma.mem_read;
    processor->dma.mem_write = wiring.dma.mem_write;
    processor->dma.scratch_rw = scratch;
    processor->dma.fifo_rw = fifo;
    /* Real uploaded instructions and DMA execute; only external RAM/FIFO input
     * is replayed. Every outgoing transfer must match the recorded payload. */
    unsigned batches = 0;
    while (dsp_has_work(processor) && batches++ < 10000) dsp_run(processor, 1000);
    CHECK(!dsp_has_work(processor));
    uint32_t end[5];
    CHECK(fread(end, sizeof(end), 1, input) == 1);
    for (unsigned i = 0; i < 5; i++) CHECK(!end[i]);
    CHECK(fread(&expected, sizeof(expected), 1, input) == 1 && fgetc(input) == EOF);
    memset((void *)processor->core.pram_opcache, 0, sizeof(processor->core.pram_opcache));
    processor->core.opaque = NULL;
    processor->core.read_peripheral = NULL; processor->core.write_peripheral = NULL;
    processor->dma.mem_opaque = processor->dma.rw_opaque = NULL;
    processor->dma.mem_read = NULL; processor->dma.mem_write = NULL;
    processor->dma.scratch_rw = NULL; processor->dma.fifo_rw = NULL;
    if (memcmp(processor, &expected, sizeof(expected))) {
        unsigned differences = 0;
        for (size_t i = 0; i < sizeof(expected); i++)
            differences += ((uint8_t *)processor)[i] != ((uint8_t *)&expected)[i];
        fprintf(stderr, "Final state mismatch: %u differing bytes; PC=%06X cycles=%u transfers=%u\n",
                differences, processor->core.pc, processor->core.cycle_count, transfers);
        for (unsigned reg = 0; reg < DSP_REG_MAX; reg++)
            if (processor->core.registers[reg] != expected.core.registers[reg])
                fprintf(stderr, "Register %02X: actual=%06X expected=%06X\n", reg,
                        processor->core.registers[reg], expected.core.registers[reg]);

        unsigned reported = 0;
        for (size_t i = 0; i < sizeof(expected); i++)
            if (((uint8_t *)processor)[i] != ((uint8_t *)&expected)[i]) {
                fprintf(stderr, "Final state differs at byte %zu: %02x != %02x\n", i,
                        ((uint8_t *)processor)[i], ((uint8_t *)&expected)[i]);
                if (++reported == 16) break;
            }
        return 1;
    }
    printf("%s full frame matched: PC=%06X cycles=%u transfers=%u\n",
           processor->is_gp ? "GP" : "EP", processor->core.pc,
           processor->core.cycle_count, transfers);
    fclose(input);
    free(processor);
    return 0;
}
