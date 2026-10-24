#ifndef XBOX_GUEST_STATE_H
#define XBOX_GUEST_STATE_H

#include <fenv.h>
#include <stdint.h>
#include "xbox_memory_layout.h"

/* The complete modeled CPU state of one guest context, and the host FP
 * environment guest code must see preserved across a nested call. Save it
 * before running a guest function from host code and restore it afterwards,
 * so the callee's volatile registers and FPU state do not leak back into the
 * caller. The caller field is left for the owner (the kernel bridge keeps its
 * own kernel-caller record there); these helpers never write it. */
typedef struct xbox_guest_state {
    uint32_t eax, ecx, edx, esp, ebx, esi, edi, ebp, seh_ebp, fs_base;
    uint32_t caller;
    int df, fp_top, fp_cmp;
    uint16_t fp_control, fp_cc;
    double fp_stack[8];
    RecompMmx mmx[8];
    RecompXmm xmm[8];
    fenv_t host_fenv;
    uint32_t mxcsr;
} xbox_guest_state;

void xbox_guest_state_save(xbox_guest_state *s);
/* Restores every field except caller; eax is the value the caller returns. */
void xbox_guest_state_restore(const xbox_guest_state *s, uint32_t eax);

#endif
