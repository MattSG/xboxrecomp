/* dsp_recomp_gen <dsp_cpu.c> <dsp_emu.c.inc> <out.inc> <program dir>
 *
 * Recompiles the DSP56300 programs a title loaded -- <dir>/gp.p.bin and
 * <dir>/ep.p.bin, saved by the runtime (dsp.c, RECOMP_DSP_PROGRAM_DIR) -- into
 * C that dsp_cpu.c includes. Each program word becomes a label running exactly
 * what dsp56k_execute_instruction would for it (DSP_RECOMP_STEP), with the
 * fetch, opcode lookup and dispatch resolved here. The code checks every word
 * against program memory before running it and hands anything else back to
 * the interpreter, so a missing, stale or different program only costs speed.
 *
 * Handler names come from the interpreter's own tables, read from its source;
 * the opcode lookup and instruction lengths come from the interpreter itself.
 * The output is rewritten only when it changes. Missing programs give stubs. */
#include "dsp_cpu.c"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *names_np[ARRAY_SIZE(nonparallel_opcodes)];
static char *names_pm[16];
static char *names_alu[256];

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "dsp_recomp_gen: cannot read %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    if (!s || fread(s, 1, n, f) != (size_t)n) exit(1);
    s[n] = 0;
    fclose(f);
    return s;
}

static char *ident(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    while ((p[n] >= 'a' && p[n] <= 'z') || (p[n] >= 'A' && p[n] <= 'Z') || (p[n] >= '0' && p[n] <= '9') || p[n] == '_') n++;
    char *s = malloc(n + 1);
    memcpy(s, p, n);
    s[n] = 0;
    return s;
}

/* nonparallel_opcodes: one { "template", "name", dis, emu[, match] } per line. */
static void read_nonparallel_names(const char *cpu_c)
{
    char *s = read_text(cpu_c);
    char *p = strstr(s, "static const OpcodeEntry nonparallel_opcodes[] = {");
    if (!p) { fprintf(stderr, "dsp_recomp_gen: opcode table not found\n"); exit(1); }
    for (size_t i = 0; i < ARRAY_SIZE(nonparallel_opcodes); i++) {
        p = strstr(p, "\n    { \"");
        if (!p) { fprintf(stderr, "dsp_recomp_gen: opcode table entry %zu not found\n", i); exit(1); }
        p += 8;
        if (strncmp(p, nonparallel_opcodes[i].template, 24) || p[24] != '"') {
            fprintf(stderr, "dsp_recomp_gen: opcode table line %zu does not match the compiled table\n", i);
            exit(1);
        }
        p += 25;                     /* template and its closing quote */
        p = strchr(p, '"') + 1;      /* name */
        p = strchr(p, '"') + 1;
        p = strchr(p, ',') + 1;      /* dis_func */
        p = strchr(p, ',') + 1;      /* emu_func */
        names_np[i] = ident(p);
    }
    free(s);
}

/* A table of function names: "<decl> = { a, b, c, ... };", comments allowed. */
static void read_name_table(const char *text, const char *decl, char **names, int count)
{
    const char *p = strstr(text, decl);
    if (!p) { fprintf(stderr, "dsp_recomp_gen: %s not found\n", decl); exit(1); }
    p = strchr(p, '{') + 1;
    for (int i = 0; i < count; i++) {
        for (;;) {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ',') p++;
            if (p[0] == '/' && p[1] == '*') { p = strstr(p, "*/") + 2; continue; }
            break;
        }
        names[i] = ident(p);
        if (!*names[i]) { fprintf(stderr, "dsp_recomp_gen: %s entry %d unreadable\n", decl, i); exit(1); }
        p += strlen(names[i]);
    }
}

static void read_emu_names(const char *emu_inc)
{
    char *s = read_text(emu_inc);
    read_name_table(s, "static const emu_func_t opcodes_parmove[16] = {", names_pm, 16);
    read_name_table(s, "static const emu_func_t opcodes_alu[256] = {", names_alu, 256);
    free(s);
}

typedef struct { char *buf; size_t len, cap; } Out;
static void out(Out *o, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(o->buf + o->len, o->cap - o->len, fmt, ap);
        va_end(ap);
        if (n >= 0 && o->len + n < o->cap) { o->len += n; return; }
        o->cap = o->cap * 2 + 4096;
        o->buf = realloc(o->buf, o->cap);
    }
}

static int program_word_compiled(const uint32_t *pram, uint32_t a, const OpcodeEntry **entry)
{
    uint32_t w = pram[a];
    *entry = NULL;
    if (w == 0 || w > 0xffffff) return 0;   /* unused memory (NOP) runs interpreted */
    if (w >= 0x100000) return 1;
    for (size_t i = 0; i < ARRAY_SIZE(nonparallel_opcodes); i++) {
        if ((w & nonparallel_matches[i][0]) != nonparallel_matches[i][1]) continue;
        if (nonparallel_opcodes[i].match_func && !nonparallel_opcodes[i].match_func(w)) continue;
        *entry = &nonparallel_opcodes[i];
        return nonparallel_opcodes[i].emu_func != NULL;
    }
    return 0;
}

/* One function per CHUNK words of program memory: inside one, labels and
 * gotos; between them, dsp_recomp_<name>'s loop. A single function for the
 * whole program grew past what the compiler optimizes (the decoding stopped
 * folding: ~5 KB of code per instruction instead of ~0.3 KB). */
#define CHUNK 256

static void emit_program(Out *o, const char *name, const uint32_t *pram, bool present)
{
    static dsp_core_t core;
    static bool compiled[DSP_PRAM_SIZE];
    static const OpcodeEntry *entries[DSP_PRAM_SIZE];
    bool chunk_used[DSP_PRAM_SIZE / CHUNK] = {0};
    for (uint32_t a = 0; a < DSP_PRAM_SIZE; a++) {
        compiled[a] = present && program_word_compiled(pram, a, &entries[a]);
        if (compiled[a]) chunk_used[a / CHUNK] = true;
    }
    memcpy(core.pram, pram, sizeof(core.pram));

    for (uint32_t c = 0; c < DSP_PRAM_SIZE / CHUNK; c++) {
        if (!chunk_used[c]) continue;
        out(o, "static int %s_%X(dsp_core_t* dsp, int budget, const uint32_t* dma_control, uint32_t dma_running, int used)\n{\n", name, c);
        out(o, "dispatch:\n    switch (dsp->pc) {\n");
        for (uint32_t a = c * CHUNK; a < (c + 1) * CHUNK; a++)
            if (compiled[a]) out(o, "    case 0x%03X: goto L_%03X;\n", a, a);
        out(o, "    default: return used;\n    }\n");
        for (uint32_t a = c * CHUNK; a < (c + 1) * CHUNK; a++) {
            if (!compiled[a]) continue;
            core.pc = a;
            core.disasm_prev_inst_pc = 0xFFFFFFFF;
            uint32_t len = disasm_instruction(&core, DSP_DISASM_MODE);
            uint32_t next = a + (len ? len : 1);
            uint32_t w = pram[a];
            out(o, "L_%03X:\n    if (dsp->pram[0x%03X] != 0x%06X) return used;\n", a, a, w);
            if (w >= 0x100000)
                out(o, "    DSP_RECOMP_STEP(0x%06X, %s_x(dsp, 0x%06X, %s));\n", w, names_pm[(w >> 20) & 0xf], w, names_alu[w & 0xff]);
            else
                out(o, "    DSP_RECOMP_STEP(0x%06X, %s(dsp));\n", w, names_np[entries[a] - nonparallel_opcodes]);
            if (next < (c + 1) * CHUNK && compiled[next])
                out(o, "    if (dsp->pc == 0x%03X) goto L_%03X;\n", next, next);
            out(o, "    goto dispatch;\n");
        }
        out(o, "}\n\n");
    }

    out(o, "static int %s(dsp_core_t* dsp, int budget, const uint32_t* dma_control, uint32_t dma_running)\n{\n", name);
    out(o, "    int used = 0;\n    for (;;) {\n        int before = used;\n        switch (dsp->pc / %d) {\n", CHUNK);
    for (uint32_t c = 0; c < DSP_PRAM_SIZE / CHUNK; c++)
        if (chunk_used[c]) out(o, "        case %u: used = %s_%X(dsp, budget, dma_control, dma_running, used); break;\n", c, name, c);
    out(o, "        default: break;\n        }\n");
    out(o, "        if (used == before || DSP_RECOMP_STOPPED) return used;\n    }\n}\n\n");
}
static bool read_program(const char *dir, const char *file, uint32_t *pram)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    bool ok = fread(pram, 4, DSP_PRAM_SIZE, f) == DSP_PRAM_SIZE;
    fclose(f);
    if (!ok) fprintf(stderr, "dsp_recomp_gen: %s is not a %d-word program; ignored\n", path, DSP_PRAM_SIZE);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: dsp_recomp_gen <dsp_cpu.c> <dsp_emu.c.inc> <out.inc> <program dir>\n");
        return 2;
    }
    static dsp_core_t init;
    dsp56k_reset_cpu(&init);          /* fills nonparallel_matches */
    read_nonparallel_names(argv[1]);
    read_emu_names(argv[2]);

    static uint32_t gp[DSP_PRAM_SIZE], ep[DSP_PRAM_SIZE];
    bool have_gp = read_program(argv[4], "gp.p.bin", gp);
    bool have_ep = read_program(argv[4], "ep.p.bin", ep);

    Out o = {0};
    out(&o, "/* Generated by dsp_recomp_gen from %s (gp %s, ep %s). Do not edit. */\n\n",
        argv[4], have_gp ? "recompiled" : "absent", have_ep ? "recompiled" : "absent");
    emit_program(&o, "dsp_recomp_gp", gp, have_gp);
    emit_program(&o, "dsp_recomp_ep", ep, have_ep);

    FILE *f = fopen(argv[3], "rb");
    if (f) {
        char *old = malloc(o.len + 1);
        size_t n = fread(old, 1, o.len + 1, f);
        fclose(f);
        bool same = n == o.len && !memcmp(old, o.buf, o.len);
        free(old);
        if (same) return 0;
    }
    f = fopen(argv[3], "wb");
    if (!f || fwrite(o.buf, 1, o.len, f) != o.len || fclose(f)) {
        fprintf(stderr, "dsp_recomp_gen: cannot write %s\n", argv[3]);
        return 1;
    }
    printf("dsp_recomp_gen: wrote %s (gp %s, ep %s)\n", argv[3], have_gp ? "recompiled" : "absent", have_ep ? "recompiled" : "absent");
    return 0;
}
