#ifndef RECOMP_CPU_H
#define RECOMP_CPU_H
#include <stdint.h>

/* Xbox Pentium III, measured through the original CPUID instruction in xemu.
 * Leaf 2 and out-of-range clamping also match xemu's Xbox cpu_x86_cpuid path.
 * The feature word reflects that machine's disabled APIC/VME, not host SIMD.
 */
static inline void recomp_cpuid(uint32_t leaf, uint32_t subleaf,
                               uint32_t *a, uint32_t *b,
                               uint32_t *c, uint32_t *d)
{
    static const uint32_t leaves[3][4] = {
        {2u, 0x756E6547u, 0x6C65746Eu, 0x49656E69u},
        {0x68Au, 0u, 0u, 0x0383F9FDu},
        {0x03020101u, 0u, 0u, 0x0C040841u}
    };
    const uint32_t *v = leaves[leaf > 2u ? 2u : leaf];
    (void)subleaf;
    /* This Xbox profile does not expose TCG hypervisor leaves. */
    if (leaf == 0x40000000u || leaf == 0x40000001u) {
        *a = *b = *c = *d = 0u;
        return;
    }
    *a = v[0]; *b = v[1]; *c = v[2]; *d = v[3];
}

static inline uint32_t recomp_flags_szp(uint32_t result, unsigned bits)
{
    uint32_t mask = UINT32_MAX >> (32u - bits);
    uint32_t value = result & mask;
    uint32_t parity = (value ^ (value >> 4)) & 15u;
    return (value == 0u ? 0x40u : 0u)
         | (value & (1u << (bits - 1u)) ? 0x80u : 0u)
         | (((0x9669u >> parity) & 1u) << 2);
}

static inline uint32_t recomp_flags_cmp(uint32_t lhs, uint32_t rhs,
                                      unsigned bits)
{
    uint32_t mask = UINT32_MAX >> (32u - bits);
    uint32_t a = lhs & mask, b = rhs & mask, result = (a - b) & mask;
    return recomp_flags_szp(result, bits) | (a < b ? 1u : 0u)
         | ((a ^ b ^ result) & 0x10u)
         | (((a ^ b) & (a ^ result) & (1u << (bits - 1u))) ? 0x800u : 0u);
}

static inline uint32_t recomp_materialize_flags(uint32_t control,
                                               uint32_t arithmetic,
                                               int direction)
{
    /* Reserved bits read zero; DF is shared with string operations. */
    return (control & 0x003F7302u) | (arithmetic & 0x8D5u)
         | (direction ? 0x400u : 0u) | 2u;
}

static inline uint32_t recomp_push_flags(uint32_t control, uint32_t arithmetic,
                                       int direction)
{
    /* PUSHFD clears VM/RF only in the pushed image. */
    return recomp_materialize_flags(control, arithmetic, direction) & ~0x30000u;
}

static inline uint32_t recomp_pop_flags(uint32_t old, uint32_t image)
{
    /* Xbox title code executes at CPL 0. Reserved, VM/VIF/VIP stay unchanged;
     * RF clears on instruction execution. The ID bit is writable on this CPU. */
    const uint32_t writable = 0x00247FD5u;
    return (old & ~(writable | 0x10000u)) | (image & writable) | 2u;
}
#endif
