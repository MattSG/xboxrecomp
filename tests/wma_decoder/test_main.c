/* WMA-to-PCM decoding through FFmpeg: the public failure contract (failures
 * leave a cleared result), and the checked-in CC0 sine fixture decoding to
 * 44.1 kHz stereo 16-bit PCM that is not silent. */
#include <stdio.h>
#include <string.h>
#include "wma_decoder.h"

static int is_cleared(const XboxWmaPcm *pcm)
{
    return pcm->data == NULL && pcm->size == 0 && pcm->sample_rate == 0 &&
           pcm->channels == 0 && pcm->bits_per_sample == 0;
}

int main(int argc, char **argv)
{
    XboxWmaPcm pcm;
    size_t i;

    /* Even argument-validation failures must honor the public failure contract
     * and clear a caller-provided result structure. */
    memset(&pcm, 0xA5, sizeof(pcm));
    if (xbox_wma_decode_file(NULL, &pcm) != -1) {
        fprintf(stderr, "NULL input unexpectedly decoded\n");
        return 1;
    }
    if (!is_cleared(&pcm)) {
        fprintf(stderr, "NULL-path failure did not leave a cleared result\n");
        return 1;
    }

    memset(&pcm, 0xA5, sizeof(pcm));
    if (xbox_wma_decode_file("__xboxrecomp_missing_wma_file__.wma", &pcm) != -1) {
        fprintf(stderr, "missing input unexpectedly decoded\n");
        return 1;
    }
    if (!is_cleared(&pcm)) {
        fprintf(stderr, "failed decode did not leave a cleared result\n");
        return 1;
    }

    /* Cleanup must remain idempotent after a failed decode. */
    xbox_wma_pcm_free(&pcm);
    xbox_wma_pcm_free(&pcm);
    if (pcm.data != NULL || pcm.size != 0) {
        fprintf(stderr, "cleanup did not remain zeroed\n");
        return 1;
    }

    if (argc != 2 || xbox_wma_decode_file(argv[1], &pcm) != 0) {
        fprintf(stderr, "fixture did not decode\n");
        return 1;
    }
    if (!pcm.data || pcm.size == 0 || pcm.size % 4 != 0 ||
        pcm.sample_rate != 44100 || pcm.channels != 2 || pcm.bits_per_sample != 16) {
        fprintf(stderr, "decoded fixture has invalid PCM or format metadata\n");
        xbox_wma_pcm_free(&pcm);
        return 1;
    }
    for (i = 0; i < pcm.size && pcm.data[i] == 0; ++i) {}
    if (i == pcm.size) {
        fprintf(stderr, "decoded sine fixture is silent\n");
        xbox_wma_pcm_free(&pcm);
        return 1;
    }
    xbox_wma_pcm_free(&pcm);
    xbox_wma_pcm_free(&pcm);

    puts("PASS: wma decoder contract and fixture");
    return 0;
}
