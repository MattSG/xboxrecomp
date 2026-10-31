#pragma once
/* Page tracker for guest address space above the slots.
 *
 * The heap (xbox_HeapAlloc, xbox_ReserveAlloc) hands out addresses inside the
 * mapped range and never honours a caller-supplied base: it gives whatever its
 * cursor is at. That is fine for a title that passes base = NULL, but one that
 * reserves a specific high address and checks it got that address back needs a
 * real answer -- the address, or a failure status its own retry logic can act
 * on -- not a different address substituted in.
 *
 * This tracks state per 4 KiB page from the top of the slots to the top of user
 * address space, backed by host memory reserved when the layout comes up and
 * committed only where a title commits.
 *
 * Opt-in, RECOMP_EXT_VMA=1. Without it nothing here is initialised and every
 * function answers "not mine" (returns 0), so the bridge behaves as before.
 * When explicitly enabled but host reservation fails, fixed upper requests
 * fail and queries report unavailable space as reserved; no heap substitution.
 *
 * Everything below the top of the slots is not this file's business. The slots
 * (the 28 map-sized ranges above the mapped range) are reserved no-access by the
 * layout, so a request that lands in them is refused here and an access faults.
 */
#include <stddef.h>
#include <stdint.h>

/* End of user address space, rounded down to a 64 KiB allocation granule. Real
 * Xbox user mode ends at 0x7FFEFFFF; the kernel and system reserve the rest. */
#define GUEST_VMEM_TOP 0x7FFE0000u

/* Called by xbox_MemoryLayoutInit once the slots are reserved. `offset` is
 * host address minus guest address; the slots occupy [slots_lo, slots_top).
 * Does nothing unless RECOMP_EXT_VMA is set, or when the slots already reach
 * the top of user space (a 128 MB map), where there is nothing above them to
 * track. Returns 1 when the tracker is live. */
/* placeholder: the layout owns a Windows placeholder above the slots. */
int guest_vmem_init(ptrdiff_t offset, uint64_t slots_lo, uint64_t slots_top,
                    int placeholder);
void guest_vmem_shutdown(void);

/* True once guest_vmem_init has succeeded. */
int guest_vmem_active(void);

/* Each returns 0 when opt-in is disabled or the address is below the slots'
 * start, so the caller uses its ordinary path. Otherwise they return 1 with
 * *status set, including explicit failure when host backing is unavailable. */
int guest_vmem_allocate(uint32_t *base, uint32_t *size, uint32_t alloc_type,
                        uint32_t protect, uint32_t *status);
int guest_vmem_free(uint32_t *base, uint32_t *size, uint32_t free_type,
                    uint32_t *status);
/* info[7] = { BaseAddress, AllocationBase, AllocationProtect, RegionSize,
 *             State, Protect, Type } -- the 28-byte guest
 *             MEMORY_BASIC_INFORMATION layout. */
int guest_vmem_query(uint32_t address, uint32_t info[7]);
