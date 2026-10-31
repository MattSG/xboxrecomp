/**
 * NV2A vertex program interpreter, for the software pushbuffer executor.
 *
 * Executes uploaded instructions on caller-supplied vertex attributes and
 * returns program output values for the software rasteriser.
 *
 * Programs and constants arrive through the pushbuffer exactly as the GPU
 * would take them (SET_TRANSFORM_PROGRAM_LOAD / _PROGRAM, _CONSTANT_LOAD /
 * _CONSTANT), so the executor forwards those methods here and nothing about
 * a title's D3D state has to be known.
 *
 * Instruction encoding: the field table at the top of nv2a_vsh_interp.c.
 * Portable C; no
 * graphics API.
 */
#ifndef XBOXRECOMP_NV2A_VSH_INTERP_H
#define XBOXRECOMP_NV2A_VSH_INTERP_H

#include <stddef.h>
#include <stdint.h>

#define NV2A_VSH_SLOTS      136     /* program memory, 128-bit instructions */
#define NV2A_VSH_CONSTANTS  192     /* c[0..191]; D3D's c0 is c[96]          */
#define NV2A_VSH_INPUTS     16      /* v0..v15                                */

/* Raw output registers, including the program's oPos value. Coordinate-space
 * interpretation belongs to the caller and uploaded program. */
typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog[4];
    float tex[4][4];
} Nv2aVshOutput;

void nv2a_vsh_set_load_slot(uint32_t slot);           /* _PROGRAM_LOAD      */
void nv2a_vsh_program_word(uint32_t word);            /* _PROGRAM(i)        */
void nv2a_vsh_set_start_slot(uint32_t slot);          /* _PROGRAM_START     */
void nv2a_vsh_set_constant_load(uint32_t index);      /* _CONSTANT_LOAD     */
void nv2a_vsh_constant_word(uint32_t word);           /* _CONSTANT(i)       */
size_t nv2a_vsh_state_size(void);
void nv2a_vsh_state_save(void *out);
void nv2a_vsh_state_restore(const void *in);
void nv2a_vsh_set_cxt_write(uint32_t enable);         /* _CXT_WRITE_EN      */

/* Run the program from the start slot on one vertex. Inputs are the sixteen
 * attribute values as floats. Returns 0 if there is no program to run (no
 * FINAL flag within program memory), which the caller treats as "cannot
 * transform this batch" rather than drawing garbage. */
int  nv2a_vsh_run(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out);

/* Direct access, for the self-test. */
void nv2a_vsh_set_constant(uint32_t index, const float v[4]);
const float *nv2a_vsh_constant(uint32_t index);   /* for traces */
/* One component of a constant, raw bits: the viewport methods write c[58]
 * (scale) and c[59] (offset) a word at a time rather than via _CONSTANT. */
void nv2a_vsh_constant_component(uint32_t index, uint32_t comp, uint32_t word);
void nv2a_vsh_set_instruction(uint32_t slot, const uint32_t words[4]);

void nv2a_vsh_debug_dump(void);

/* Program memory and constants, with change counters, for a GPU translation. */
const uint32_t (*nv2a_vsh_program(uint32_t *start, uint32_t *version))[4];
const float (*nv2a_vsh_constants(uint32_t *version))[4];
#endif
