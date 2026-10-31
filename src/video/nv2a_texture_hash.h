/* Exact texture-content hash. Native integer lanes preserve the scalar hash;
 * only CPU execution changes, never the amount or frequency of validation. */
#ifndef NV2A_TEXTURE_HASH_H
#define NV2A_TEXTURE_HASH_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#include <immintrin.h>
#ifndef NV_HASH_CPUID
#define NV_HASH_CPUID(result,leaf,subleaf) __cpuidex(result,leaf,subleaf)
#endif
#ifndef NV_HASH_XGETBV
#define NV_HASH_XGETBV(index) _xgetbv(index)
#endif
static int hash_mem_cpu_native(void)
{
    int r[4];
    NV_HASH_CPUID(r,0,0);
    if(r[0]<7) return 0;
    NV_HASH_CPUID(r,1,0);
    /* XSAVE, OSXSAVE and AVX precede XGETBV. The OS must save XMM/YMM,
     * opmask and both upper ZMM components, including for 256-bit EVEX. */
    if(((uint32_t)r[2]&0x1C000000u)!=0x1C000000u) return 0;
    if((NV_HASH_XGETBV(0)&0xE6u)!=0xE6u) return 0;
    NV_HASH_CPUID(r,7,0);
    /* AVX2 for the 256-bit logical shifts, AVX512F/DQ/VL for VPMULLQ. */
    return ((uint32_t)r[1]&0x80030020u)==0x80030020u;
}
static volatile long s_hash_mem_native = -1;
static int hash_mem_native_available(void)
{
    long available=s_hash_mem_native;
    if(available<0) {
        int detected=hash_mem_cpu_native();
        _InterlockedCompareExchange(&s_hash_mem_native,detected,-1);
        available=s_hash_mem_native;
    }
    return available!=0;
}
/* Keep feature instructions outside the generic scalar function. No global
 * /arch option is required or used; the runtime guard owns every call here. */
static __declspec(noinline) uint64_t hash_mem_native(const uint8_t *p,size_t n)
{
    __m256i lanes=_mm256_set_epi64x(0x27D4EB2F165667C5ull^n,0x165667B19E3779F9ull,
                                   0xC2B2AE3D27D4EB4Full,0x9E3779B97F4A7C15ull);
    __m256i prime=_mm256_set1_epi64x(0x100000001B3ull);
    uint64_t w[4];
    size_t i=0;
    for(;i+32<=n;i+=32) {
        lanes=_mm256_mullo_epi64(_mm256_xor_si256(lanes,
                   _mm256_loadu_si256((const __m256i *)(p+i))),prime);
        lanes=_mm256_xor_si256(lanes,_mm256_srli_epi64(lanes,31));
    }
    _mm256_storeu_si256((__m256i *)w,lanes);
    for(;i<n;i++) w[0]=(w[0]^p[i])*0x100000001B3ull;
    return w[0]^(w[1]*3)^(w[2]*5)^(w[3]*7);
}
#endif

#ifndef NV_HASH_NATIVE_MIN
#define NV_HASH_NATIVE_MIN 1024u
#endif
static uint64_t hash_mem(const uint8_t *p,size_t n)
{
    uint64_t a=0x9E3779B97F4A7C15ull,b=0xC2B2AE3D27D4EB4Full,
             c=0x165667B19E3779F9ull,d=0x27D4EB2F165667C5ull^n;
    size_t i=0;
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    if(n>=NV_HASH_NATIVE_MIN && hash_mem_native_available()) return hash_mem_native(p,n);
#endif
    for(;i+32<=n;i+=32) {
        uint64_t w[4];memcpy(w,p+i,32);
        a=(a^w[0])*0x100000001B3ull;a^=a>>31;
        b=(b^w[1])*0x100000001B3ull;b^=b>>31;
        c=(c^w[2])*0x100000001B3ull;c^=c>>31;
        d=(d^w[3])*0x100000001B3ull;d^=d>>31;
    }
    for(;i<n;i++) a=(a^p[i])*0x100000001B3ull;
    return a^(b*3)^(c*5)^(d*7);
}
#endif
