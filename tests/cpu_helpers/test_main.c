#include "recomp_cpu.h"
#include <stdio.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "failed: %s\n", #condition); return 1; } } while (0)

int main(void)
{
    uint32_t a, b, c, d;
    recomp_cpuid(0, 0, &a, &b, &c, &d);
    CHECK(a == 2 && b == 0x756E6547 && c == 0x6C65746E && d == 0x49656E69);
    recomp_cpuid(1, 0, &a, &b, &c, &d);
    CHECK(a == 0x68A && b == 0 && c == 0 && d == 0x0383F9FD);
    recomp_cpuid(UINT32_MAX, 0, &a, &b, &c, &d);
    CHECK(a == 0x03020101 && d == 0x0C040841);
    recomp_cpuid(0x40000000, 0, &a, &b, &c, &d);
    CHECK((a | b | c | d) == 0);
    recomp_cpuid(0x40000001, 0, &a, &b, &c, &d);
    CHECK((a | b | c | d) == 0);
    CHECK(recomp_flags_szp(0x100, 8) == 0x44);
    CHECK(recomp_flags_cmp(0, 1, 8) == 0x95);
    CHECK(recomp_flags_cmp(0x80, 1, 8) == 0x810);
    CHECK(recomp_flags_cmp(0x8000, 1, 16) == 0x814);
    CHECK(recomp_flags_cmp(0x80000000, 1, 32) == 0x814);
    CHECK(recomp_materialize_flags(0, 0x8D5, 1) == 0xCD7);
    CHECK((recomp_push_flags(0x30000, 0, 0) & 0x30000) == 0);
    CHECK(recomp_pop_flags(0x10000, 0x200001) == 0x200003);
    CHECK((recomp_pop_flags(0x20000, 0) & 0x20000) != 0);
    puts("PASS: Xbox CPUID and guest flags");
    return 0;
}
