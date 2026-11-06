/* RECOMP_KEYMAP entries: whole-name lookup, hex values, and appending. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "keymap_spec.h"

int main(void)
{
    const char *spec = "RLX:4D,LX-:25,A:5A";
    unsigned value = 0;
    char out[32] = "";

    assert(keymap_spec_find(spec, "LX-", &value) && value == 0x25);
    assert(keymap_spec_find(spec, "RLX", &value) && value == 0x4D);
    assert(!keymap_spec_find(spec, "LX", &value));   /* a prefix is not an entry */
    assert(!keymap_spec_find(spec, "X", &value));    /* nor is a suffix */
    assert(!keymap_spec_find("", "A", &value));
    assert(keymap_spec_find("A:5A", "A", &value) && value == 0x5A);

    assert(keymap_spec_append(out, sizeof out, "RT", 0x33));
    assert(keymap_spec_append(out, sizeof out, "LT", 0x31));
    assert(!strcmp(out, "RT:33,LT:31"));
    assert(!keymap_spec_append(out, sizeof out, "A_NAME_LONGER_THAN_THE_REST_OF_THE_BUFFER_ALLOWS", 1));
    assert(!keymap_spec_append(out, 4, "RT", 0x33));       /* smaller than the text: unchanged */
    assert(!strcmp(out, "RT:33,LT:31"));

    printf("PASS: keymap entries match whole names, parse hex values and append in order\n");
    return 0;
}
