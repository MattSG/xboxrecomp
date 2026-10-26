#include "guest_state.h"
#include <string.h>
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <xmmintrin.h>
#define GUEST_STATE_HAS_MXCSR 1
#endif

/* The recompiler's per-thread guest register file, as kernel_bridge.c declares it. */
extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern RECOMP_TLS uint32_t g_seh_ebp;
extern RECOMP_TLS uint32_t g_ebp;
extern RECOMP_TLS uint32_t g_fs_base;
extern RECOMP_TLS int g_df, g_fp_top, g_fp_cmp;
extern RECOMP_TLS uint16_t g_fp_control_word, g_fp_cc;
extern RECOMP_TLS double g_fp_stack[8];
extern RECOMP_TLS RecompMmx g_mm0, g_mm1, g_mm2, g_mm3;
extern RECOMP_TLS RecompMmx g_mm4, g_mm5, g_mm6, g_mm7;
extern RECOMP_TLS RecompXmm g_xmm0, g_xmm1, g_xmm2, g_xmm3;
extern RECOMP_TLS RecompXmm g_xmm4, g_xmm5, g_xmm6, g_xmm7;

void xbox_guest_state_save(xbox_guest_state *s)
{
    s->eax=g_eax; s->ecx=g_ecx; s->edx=g_edx; s->esp=g_esp;
    s->ebx=g_ebx; s->esi=g_esi; s->edi=g_edi; s->ebp=g_ebp;
    s->seh_ebp=g_seh_ebp; s->fs_base=g_fs_base;
    s->df=g_df; s->fp_top=g_fp_top; s->fp_cmp=g_fp_cmp;
    s->fp_control=g_fp_control_word; s->fp_cc=g_fp_cc;
    memcpy(s->fp_stack,g_fp_stack,sizeof(s->fp_stack));
    s->mmx[0]=g_mm0; s->mmx[1]=g_mm1; s->mmx[2]=g_mm2; s->mmx[3]=g_mm3;
    s->mmx[4]=g_mm4; s->mmx[5]=g_mm5; s->mmx[6]=g_mm6; s->mmx[7]=g_mm7;
    s->xmm[0]=g_xmm0; s->xmm[1]=g_xmm1; s->xmm[2]=g_xmm2; s->xmm[3]=g_xmm3;
    s->xmm[4]=g_xmm4; s->xmm[5]=g_xmm5; s->xmm[6]=g_xmm6; s->xmm[7]=g_xmm7;
    fegetenv(&s->host_fenv);
#ifdef GUEST_STATE_HAS_MXCSR
    s->mxcsr=_mm_getcsr();
#else
    s->mxcsr=0;
#endif
}

void xbox_guest_state_restore(const xbox_guest_state *s, uint32_t eax)
{
    g_eax=eax; g_ecx=s->ecx; g_edx=s->edx; g_esp=s->esp;
    g_ebx=s->ebx; g_esi=s->esi; g_edi=s->edi; g_ebp=s->ebp;
    g_seh_ebp=s->seh_ebp; g_fs_base=s->fs_base;
    g_df=s->df; g_fp_top=s->fp_top; g_fp_cmp=s->fp_cmp;
    g_fp_control_word=s->fp_control; g_fp_cc=s->fp_cc;
    memcpy(g_fp_stack,s->fp_stack,sizeof(s->fp_stack));
    g_mm0=s->mmx[0]; g_mm1=s->mmx[1]; g_mm2=s->mmx[2]; g_mm3=s->mmx[3];
    g_mm4=s->mmx[4]; g_mm5=s->mmx[5]; g_mm6=s->mmx[6]; g_mm7=s->mmx[7];
    g_xmm0=s->xmm[0]; g_xmm1=s->xmm[1]; g_xmm2=s->xmm[2]; g_xmm3=s->xmm[3];
    g_xmm4=s->xmm[4]; g_xmm5=s->xmm[5]; g_xmm6=s->xmm[6]; g_xmm7=s->xmm[7];
    fesetenv(&s->host_fenv);
#ifdef GUEST_STATE_HAS_MXCSR
    _mm_setcsr(s->mxcsr);
#endif
}

void xbox_callee_saved_save(xbox_callee_saved *s)
{
    s->ebx = g_ebx;
    s->esi = g_esi;
    s->edi = g_edi;
    s->ebp = g_ebp;
    s->seh_ebp = g_seh_ebp;
}

void xbox_callee_saved_restore(const xbox_callee_saved *s)
{
    g_ebx = s->ebx;
    g_esi = s->esi;
    g_edi = s->edi;
    g_ebp = s->ebp;
    g_seh_ebp = s->seh_ebp;
}
