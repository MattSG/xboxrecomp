#include "dsp_port.h"
#include "dsp_dma.h"
#include "dsp_dma_regs.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "DMA comparison line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static uint32_t memory[128];
static uint8_t buffer[128];
static unsigned transfers;
static uint32_t read_word(void *opaque, int space, uint32_t addr)
{
    (void)opaque;
    CHECK(space == 0 && addr < ARRAY_SIZE(memory));
    return memory[addr];
}
static void write_word(void *opaque, int space, uint32_t addr, uint32_t value)
{
    (void)opaque;
    CHECK(space == 0 && addr < ARRAY_SIZE(memory));
    memory[addr] = value;
}
static void scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    (void)opaque;
    CHECK(addr <= sizeof(buffer) && len <= sizeof(buffer) - addr);
    if (dir) memcpy(buffer + addr, ptr, len);
    else memcpy(ptr, buffer + addr, len);
    transfers++;
}
static void fifo_rw(void *opaque, uint8_t *ptr, unsigned index, size_t len, bool dir)
{
    CHECK(index < 4 && dir);
    scratch_rw(opaque, ptr, 0, len, dir);
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    FILE *output = fopen(argv[1], "wb");
    CHECK(output);
    unsigned cases = 0;
    /* Only common implemented modes: compare payload/memory/EOL/next pointer,
     * not upstream's synchronous scheduling or read-count completion timer. */
    for (unsigned format = 1; format <= 6; format += 5)
    for (unsigned circular = 0; circular < 2; circular++)
    for (unsigned direction = 0; direction < 2; direction++)
    for (unsigned interleave = 0; interleave < 2; interleave++) {
        if (interleave && !direction) continue;
        for (unsigned i = 0; i < ARRAY_SIZE(memory); i++) memory[i] = (0x801234 + i * 0x12345) & 0xffffff;
        for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = (uint8_t)(i * 37 + 11);
        DSPDMAState dma = {0};
        dma.mem_read = read_word;
        dma.mem_write = write_word;
        dma.scratch_rw = scratch_rw;
        dma.fifo_rw = fifo_rw;
        memory[0] = NODE_POINTER_EOL;
        memory[1] = ((circular ? 0xe : 0xf) << 5) | (format << 10) |
                    (direction ? NODE_CONTROL_DIRECTION : 0) | interleave | (1 << 4);
        memory[2] = interleave ? 0x22 : 6; /* two samples, three channels */
        memory[3] = 32;
        memory[4] = circular ? 60 : 4;
        memory[5] = 0;
        memory[6] = 63;
        transfers = 0;
        dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0);
        dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
#ifndef DSP_REFERENCE_DMA
        CHECK(transfers == 0 && !dma.eol);
        dsp_dma_step(&dma);
#endif
        CHECK(transfers && dma.eol && !dma.error);
        CHECK(fwrite(memory, sizeof(memory), 1, output) == 1);
        CHECK(fwrite(buffer, sizeof(buffer), 1, output) == 1);
        CHECK(fwrite(&dma.next_block, sizeof(dma.next_block), 1, output) == 1);
        cases++;
    }
    /* Common FIFO output, with nontrivial planar-to-interleaved ordering. */
    for (unsigned index = 0; index < 4; index++) {
        DSPDMAState dma = {0};
        dma.mem_read = read_word; dma.mem_write = write_word;
        dma.scratch_rw = scratch_rw; dma.fifo_rw = fifo_rw;
        memory[1] = (index << 5) | (1 << 10) | NODE_CONTROL_DIRECTION | 1;
        memory[2] = 0x22;
        memset(buffer, 0, sizeof(buffer));
        dsp_dma_write(&dma, DMA_NEXT_BLOCK, 0);
        dsp_dma_write(&dma, DMA_CONTROL, DMA_CONTROL_ACTION_START);
#ifndef DSP_REFERENCE_DMA
        dsp_dma_step(&dma);
#endif
        CHECK(dma.eol && !dma.error);
        CHECK(fwrite(buffer, sizeof(buffer), 1, output) == 1);
        cases++;
    }
    CHECK(fclose(output) == 0);
    printf("%u DMA transfer cases saved\n", cases);
    return 0;
}
