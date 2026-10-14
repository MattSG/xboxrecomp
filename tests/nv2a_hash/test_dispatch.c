/* Each benchmark candidate compiles the actual production dispatcher at a
 * different minimum size; no alternate scalar or feature implementation. */
#include "nv2a_texture_hash.h"
__declspec(noinline) uint64_t NV_HASH_TEST_NAME(const uint8_t *p,size_t n)
{
    return hash_mem(p,n);
}
