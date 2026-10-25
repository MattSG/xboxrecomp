/* x87 PC=24: results round to a 24-bit significand, exponent range intact. */
#include "recomp_types.h"
#include <float.h>
#include <stdio.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"FAIL line %d\n",__LINE__); return 1; } } while(0)
ptrdiff_t g_xbox_mem_offset;
static uint8_t guest[16];
/* fld tbyte at guest address 0: the 80-bit operand's bytes, little-endian. */
static double fp80(uint64_t significand, uint16_t sign_exponent) {
    int i;
    for (i = 0; i < 8; ++i) guest[i] = (uint8_t)(significand >> (8 * i));
    guest[8] = (uint8_t)sign_exponent; guest[9] = (uint8_t)(sign_exponent >> 8);
    return MEMFP80(0);
}
int main(void) {
    g_xbox_mem_offset = (ptrdiff_t)(uintptr_t)guest;
    CHECK(fp80(UINT64_C(0x8000000000000000), 0x3FFF) == 1.0);
    CHECK(fp80(UINT64_C(0xC000000000000000), 0xC000) == -3.0);
    CHECK(fp80(UINT64_C(0xC90FDAA22168C235), 0x4000) == 3.14159265358979323846);
    CHECK(fp80(0, 0) == 0.0);
    CHECK(isinf(fp80(UINT64_C(0x8000000000000000), 0xFFFF)));
    CHECK(fp80(UINT64_C(0x8000000000000000), 0xFFFF) < 0);
    CHECK(isnan(fp80(UINT64_C(0xC000000000000000), 0x7FFF)));
    /* The Burnout 3 case: 96 * (float)(1/255) must equal its float-stored copy. */
    double v = 96.0 * (double)(1.0f / 255.0f);
    CHECK(recomp_fp_round24(v) == (double)(float)v);
    CHECK(v != (double)(float)v);           /* ...which double precision is not */
    /* Past float's range the x87 keeps going; a (float) cast would not. */
    CHECK(recomp_fp_round24(1e300) == ldexp((double)(float)frexp(1e300, &(int){0}), 997));
    CHECK(isfinite(recomp_fp_round24(1e300)) && recomp_fp_round24(1e300) * 0.0 == 0.0);
    CHECK(recomp_fp_round24(1e-300) != 0.0);
    CHECK(recomp_fp_round24(-3.0) == -3.0 && recomp_fp_round24(0.0) == 0.0);
    CHECK(isinf(recomp_fp_round24(INFINITY)));
    CHECK(isnan(recomp_fp_round24(NAN)));
    puts("ok");
    return 0;
}
