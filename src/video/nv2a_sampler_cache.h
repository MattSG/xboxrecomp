#ifndef XBR_NV2A_SAMPLER_CACHE_H
#define XBR_NV2A_SAMPLER_CACHE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t key;
    void *resource;
} Nv2aSamplerSlot;

/* slot_count must be a power of two. On a full-cache miss, choose a slot whose
 * current resource is not referenced by the draw being assembled. Replacing
 * an occupied slot keeps the open-addressed table searchable. */
static size_t nv2a_sampler_slot(Nv2aSamplerSlot *slots, size_t slot_count,
                                size_t home, uint64_t key,
                                void *const *pinned, size_t pinned_count,
                                int *found)
{
    size_t n, first_empty = slot_count;
    *found = 0;
    for (n = 0; n < slot_count; ++n) {
        size_t i = (home + n) & (slot_count - 1);
        if (slots[i].resource) {
            if (slots[i].key == key) {
                *found = 1;
                return i;
            }
        } else {
            first_empty = i;
            break;
        }
    }
    if (first_empty != slot_count) return first_empty;

    for (n = 0; n < slot_count; ++n) {
        size_t i = (home + n) & (slot_count - 1), p;
        int is_pinned = 0;
        for (p = 0; p < pinned_count; ++p)
            if (slots[i].resource == pinned[p]) {
                is_pinned = 1;
                break;
            }
        if (!is_pinned) return i;
    }
    return slot_count;
}

#endif
