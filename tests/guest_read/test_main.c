#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "guest_read.h"

static unsigned calls;

int xbox_GuestRead(uint32_t va, void *out, size_t size)
{
    const uint32_t value = 0x12345678;
    ++calls;
    if (va != 0x10000 || size != sizeof value)
        return 0;
    memcpy(out, &value, size);
    return 1;
}

int main(void)
{
    if (xbox_guest_word_at(0x10000) != 0x12345678 || calls != 1 ||
        xbox_guest_word_at(0x20000) != 0 || calls != 2 ||
        xbox_guest_word_at(UINT64_C(0x100000000)) != 0 || calls != 2)
        return 1;
    puts("guest_read: readable, unreadable and oversized addresses passed");
    return 0;
}
