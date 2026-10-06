/* NV2A vertex program -> HLSL (nv2a_vsh_hlsl.c). */
#ifndef XBOXRECOMP_NV2A_VSH_HLSL_H
#define XBOXRECOMP_NV2A_VSH_HLSL_H

#include <stddef.h>
#include <stdint.h>

/* How an attribute reaches the shader from its D3D11 input format. */
enum {
    NV_VSH_IN_FLOAT4,     /* float / UNORM / SNORM, missing components (0,0,0,1) */
    NV_VSH_IN_FLOAT3W1,   /* four components fetched, w forced to 1 */
    NV_VSH_IN_INT4,       /* SINT, converted to float */
    NV_VSH_IN_INT3W1,
    NV_VSH_IN_CMP         /* packed 11:11:10 normal in a uint */
};

typedef struct {
    uint8_t enabled;      /* 0: the shader reads DEF[i] (current value) */
    uint8_t kind;
} NvVshInput;

/* 1 if the program cannot run on the GPU (writes constants, or no FINAL). */
int nv2a_vsh_needs_cpu(const uint32_t (*prog)[4], uint32_t start, uint32_t slots);

/* Writes HLSL with entry point `main`. Returns its length, 0 if the program
 * has no FINAL instruction, -1 if `cap` was too small. */
int nv2a_vsh_to_hlsl(const uint32_t (*prog)[4], uint32_t start, uint32_t slots,
                     const NvVshInput in[16], char *out, size_t cap);

#endif
