#include <assert.h>
#include <stdint.h>
#include <string.h>

int xbox_FramebufferPaused(void) { return 0; }
#include "../../src/apu/apu_sdl.c"

int main(void)
{
    int16_t input[256][2], output[256][2];
    int low, high;
    for (int i = 0; i < 256; ++i) {
        input[i][0] = (int16_t)i;
        input[i][1] = (int16_t)-i;
    }
    assert(!xa2_submit_samples(&input[0][0], 256));
    for (int cycle = 0; cycle < 2; ++cycle) {
        assert(xa2_init());
        assert(xa2_init());
        assert(xa2_is_active());
        assert(SDL_PauseAudioStreamDevice(g_dev));
        assert(!xa2_submit_samples(&input[0][0], 0));
        assert(xa2_submit_samples(&input[0][0], 256));
        assert(xa2_queued_samples() == 256);
        assert(g_frames_written == 1);
        memcpy(output, input, sizeof input);
        memset(input, 0, sizeof input);
        int16_t received[256][2];
        assert(SDL_GetAudioStreamData(g_dev, received, sizeof received) == sizeof received);
        assert(!memcmp(received, output, sizeof output));
        memcpy(input, output, sizeof input);
        xa2_pacing_marks(xa2_get_buffer_size(), xa2_get_buffer_count(), 256, &low, &high);
        assert(low > 0 && high >= low);
        xa2_shutdown();
        xa2_shutdown();
        assert(!xa2_is_active());
        assert(!xa2_submit_samples(&input[0][0], 256));
    }
    return 0;
}
