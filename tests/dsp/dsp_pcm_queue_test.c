#include "apu_state.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "PCM queue check failed at line %d: %s\n", __LINE__, #condition); \
    exit(1); } } while (0)

struct McpxApuDebug g_dbg;
int dsp_test_fail_alloc;
uint8_t *g_apu_ram_ptr;
uint8_t *mcpx_apu_phys(uint64_t address)
{
    (void)address;
    CHECK(0); /* Queue-only checks must not read or write guest memory. */
    return NULL;
}

extern uint64_t dsp_test_pcm_allocations, dsp_test_pcm_copied, dsp_test_pcm_moved;
void dsp_test_publish_pcm_frame(MCPXAPUState *d, const uint8_t *pcm);

static void fill_frame(uint8_t *frame, unsigned sequence)
{
    for (unsigned i = 0; i < 1024; i++)
        frame[i] = (uint8_t)(sequence * 17 + i * 23 + (sequence >> 8));
}

static void publish(MCPXAPUState *d, unsigned sequence)
{
    uint8_t frame[1024];
    fill_frame(frame, sequence);
    dsp_test_publish_pcm_frame(d, frame);
}

static void check_frame(MCPXAPUState *d, unsigned sequence)
{
    uint8_t frame[1024];
    fill_frame(frame, sequence);
    CHECK(!memcmp(d->monitor.frame_buf, frame, sizeof(frame)));
}

int main(void)
{
    MCPXAPUState *d = calloc(1, sizeof(*d));
    CHECK(d);

    /* Move the head, wrap at the old end, fail a growth, then grow the
     * wrapped queue. Every retained frame must remain in its original order. */
    for (unsigned i = 0; i < 5; i++) publish(d, i);
    CHECK(d->monitor.ep_pcm_capacity == 4096);
    for (unsigned i = 0; i < 3; i++) {
        check_frame(d, i);
        mcpx_apu_dsp_pcm_consumed(d);
    }
    for (unsigned i = 5; i < 8; i++) publish(d, i);
    uint64_t dropped = d->monitor.ep_pcm_dropped_frames;
    dsp_test_fail_alloc = 1;
    publish(d, 99);
    dsp_test_fail_alloc = 0;
    CHECK(d->monitor.ep_pcm_capacity == 4096);
    CHECK(d->monitor.ep_pcm_queued == 4096);
    CHECK(d->monitor.ep_pcm_dropped_frames == dropped + 1);
    publish(d, 8);
    CHECK(d->monitor.ep_pcm_capacity == 8192);
    for (unsigned i = 3; i < 9; i++) {
        check_frame(d, i);
        mcpx_apu_dsp_pcm_consumed(d);
    }
    CHECK(!d->monitor.ep_pcm_ready && !d->monitor.ep_pcm_queued);
    CHECK(!d->monitor.ep_pcm_head);

    /* Compare a stream of bursts, consumes and overflows to an independent
     * FIFO model. The model stores sequence numbers rather than PCM offsets. */
    unsigned model[65], count = 0;
    uint32_t random = 0x762d81ab;
    for (unsigned sequence = 0; sequence < 20000; sequence++) {
        random = random * 1664525u + 1013904223u;
        if ((random >> 28) < 5) {
            if (count) {
                check_frame(d, model[0]);
                memmove(model, model + 1, --count * sizeof(*model));
            }
            mcpx_apu_dsp_pcm_consumed(d);
        } else {
            uint64_t drops_before = d->monitor.ep_pcm_dropped_frames;
            publish(d, sequence);
            if (count < 65) {
                model[count++] = sequence;
                CHECK(d->monitor.ep_pcm_dropped_frames == drops_before);
            } else CHECK(d->monitor.ep_pcm_dropped_frames == drops_before + 1);
        }
        CHECK(d->monitor.ep_pcm_ready == (count != 0));
        CHECK(d->monitor.ep_pcm_queued == (count ? count - 1 : 0) * 1024);
        CHECK(d->monitor.ep_pcm_capacity <= 65536);
        if (count) check_frame(d, model[0]);
    }
    while (count) {
        check_frame(d, model[0]);
        memmove(model, model + 1, --count * sizeof(*model));
        mcpx_apu_dsp_pcm_consumed(d);
    }

    /* Consumption copies only the next frame even with a full backlog.
     * Allocation growth from zero through 64 frames takes seven allocations. */
    free(d->monitor.ep_pcm_queue);
    memset(&d->monitor, 0, sizeof(d->monitor));
    dsp_test_pcm_allocations = dsp_test_pcm_copied = dsp_test_pcm_moved = 0;
    for (unsigned i = 0; i < 65; i++) publish(d, i);
    CHECK(dsp_test_pcm_allocations == 7);
    CHECK(d->monitor.ep_pcm_queued == 65536);
    dsp_test_pcm_copied = dsp_test_pcm_moved = 0;
    for (unsigned i = 0; i < 65; i++) {
        check_frame(d, i);
        mcpx_apu_dsp_pcm_consumed(d);
    }
    CHECK(dsp_test_pcm_copied == 65536 && !dsp_test_pcm_moved);
    printf("PCM queue: ordered wrap/growth/OOM/bursts; 7 growth allocations, "
           "65536 copied bytes and 0 shifted bytes for a 64-frame drain\n");
    free(d->monitor.ep_pcm_queue);
    free(d);
    return 0;
}
