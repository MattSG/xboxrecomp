#include "guest_read.h"
#include "xbox_memory_layout.h"

uint32_t xbox_guest_word_at(uint64_t va)
{
    uint32_t value = 0;
    if (va > UINT32_MAX)
        return 0;
    xbox_GuestRead((uint32_t)va, &value, sizeof value);
    return value;
}
