#include "keymap_spec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int keymap_spec_find(const char *spec, const char *name, unsigned *value)
{
    size_t length = strlen(name);
    const char *entry = spec;
    while (entry && *entry) {
        const char *end = strchr(entry, ',');
        size_t size = end ? (size_t)(end - entry) : strlen(entry);
        if (size > length + 1 && entry[length] == ':' && !strncmp(entry, name, length)) {
            if (value) *value = (unsigned)strtoul(entry + length + 1, NULL, 16);
            return 1;
        }
        entry = end ? end + 1 : NULL;
    }
    return 0;
}

int keymap_spec_append(char *spec, size_t capacity, const char *name, unsigned value)
{
    size_t used = strlen(spec);
    int written;
    if (used >= capacity) return 0;
    written = snprintf(spec + used, capacity - used, "%s%s:%X",
                           used ? "," : "", name, value);
    if (written < 0 || (size_t)written >= capacity - used) {
        spec[used] = 0;
        return 0;
    }
    return 1;
}
