#include "nv2a_sampler_cache.h"

#include <stdio.h>
#include <string.h>

#define SLOTS 1024

static int fail(const char *message)
{
    fprintf(stderr, "sampler cache regression: %s\n", message);
    return 1;
}

int main(int argc, char **argv)
{
    Nv2aSamplerSlot slots[SLOTS];
    unsigned token[SLOTS + 1];
    int released[SLOTS + 1];
    void *pinned[2], *graveyard[1], *bound[2];
    size_t i, victim, graveyard_count = 0;
    int found = 0, legacy = argc > 1 && strcmp(argv[1], "--legacy") == 0;

    memset(released, 0, sizeof released);
    for (i = 0; i < SLOTS; ++i) {
        slots[i].key = i + 1;
        slots[i].resource = &token[i];
    }

    /* The in-progress draw already collected these two sampler objects. */
    pinned[0] = slots[0].resource;
    pinned[1] = slots[1].resource;

    /* A full-cache miss hashing to slot zero reproduces the setup_pipeline
     * pressure case. The old policy always selected home, which is pinned. */
    victim = legacy ? 0 : nv2a_sampler_slot(slots, SLOTS, 0, SLOTS + 7,
                                            pinned, 2, &found);
    if (legacy && victim != 0) return fail("legacy control did not select home");
    if (victim >= SLOTS || found) return fail("full-cache miss did not select an eviction slot");
    if (slots[victim].resource == pinned[0] || slots[victim].resource == pinned[1])
        return fail("eviction selected a sampler pinned by the current draw");

    graveyard[graveyard_count++] = slots[victim].resource;
    slots[victim].key = SLOTS + 7;
    slots[victim].resource = &token[SLOTS];

    /* setup_pipeline now rebinds targets and flushes; that retires the
     * evicted sampler while current-draw samplers must remain usable. */
    for (i = 0; i < graveyard_count; ++i) {
        size_t id = (unsigned *)graveyard[i] - token;
        if (id > SLOTS) return fail("graveyard held an unknown sampler");
        released[id] = 1;
    }
    bound[0] = pinned[0];
    bound[1] = pinned[1];
    for (i = 0; i < 2; ++i) {
        size_t id = (unsigned *)bound[i] - token;
        if (id >= SLOTS || released[id])
            return fail("current draw bound a sampler released by the mid-setup flush");
    }
    return 0;
}
