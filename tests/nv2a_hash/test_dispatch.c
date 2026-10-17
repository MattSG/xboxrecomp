/* Each benchmark candidate compiles the actual production dispatcher at a
 * different minimum size; no alternate scalar or feature implementation. */
#include "nv2a_texture_hash.h"
#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
NOINLINE uint64_t NV_HASH_TEST_NAME(const uint8_t *p,size_t n)
{
    return hash_mem(p,n);
}
