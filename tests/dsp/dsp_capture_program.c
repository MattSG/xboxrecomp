/* dsp_capture_program <capture.frame.bin> <dir>: write the captured
 * processor's program memory as <dir>/gp.p.bin or ep.p.bin, the input
 * tools/dsp_recomp takes (the runtime saves it from a running title). */
#include "dsp.h"
#include "dsp_port.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "capture program line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

int main(int argc, char **argv)
{
    static DSPState state;
    uint32_t header[3];
    CHECK(argc == 3);
    FILE *in = fopen(argv[1], "rb");
    CHECK(in && fread(header, sizeof(header), 1, in) == 1);
    CHECK(header[0] == 0x44535046 && header[1] == DSP_FRAME_CAPTURE_VERSION && header[2] == sizeof(state));
    CHECK(fread(&state, sizeof(state), 1, in) == 1);
    fclose(in);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.p.bin", argv[2], state.is_gp ? "gp" : "ep");
    FILE *out = fopen(path, "wb");
    CHECK(out && fwrite(state.core.pram, 4, DSP_PRAM_SIZE, out) == DSP_PRAM_SIZE && !fclose(out));
    return 0;
}
