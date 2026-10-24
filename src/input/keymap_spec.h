/* The RECOMP_KEYMAP setting format: a comma-separated list of NAME:HEX entries,
 * each name a key and each value the key's virtual-key code in hex. Names
 * match whole entries only, so LX- never matches inside RLX-. */
#ifndef INPUT_KEYMAP_SPEC_H
#define INPUT_KEYMAP_SPEC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reads NAME's value from spec into value and returns 1; returns 0 when the
 * spec has no entry for NAME. */
int keymap_spec_find(const char *spec, const char *name, unsigned *value);

/* Appends NAME:HEX to spec, comma-separated after any existing entry. Returns
 * 1 when the entry fit in capacity bytes, 0 when it did not (spec unchanged). */
int keymap_spec_append(char *spec, size_t capacity, const char *name, unsigned value);

#ifdef __cplusplus
}
#endif

#endif
