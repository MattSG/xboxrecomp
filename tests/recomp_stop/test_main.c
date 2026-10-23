/* Residual translation failures must stop, not fall through.
 *
 * A residual dead branch (RECOMP_DEAD_BRANCH, the runtime default) and an
 * unresolved call target (recomp_unresolved_stub_stop) both abort. Child
 * processes prove each one stops before a fallthrough sentinel runs, and that
 * the diagnostics do not dereference an unmapped stack.
 * MSVC only: the child-process check uses _spawnl and _set_abort_behavior.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "recomp_types.h"

/* The runtime owns g_esp; the generated dispatch tables are absent here. */
ptrdiff_t g_xbox_mem_offset;
recomp_func_t recomp_lookup(uint32_t va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return 0; }
#define CHECK(x) do { if (!(x)) { fprintf(stdout,"%s:%d: failed: %s\n",__FILE__,__LINE__,#x); return 1; } } while (0)

static int jump(int cond)
{
    if (cond) RECOMP_DEAD_BRANCH(0x0009398Fu);
    return 99;
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        CHECK(freopen("recomp_stop_stderr.txt", "w", stderr));
        g_esp = 0x10000u; /* Unmapped: diagnostics must not dereference it. */
        if (!strcmp(argv[1], "stub")) recomp_unresolved_stub_stop(0x123456u);
        else return jump(1);
        return 99;
    }
    CHECK(jump(0) == 99);
    for (int mode = 0; mode < 2; ++mode) {
        const char *argument = mode ? "stub" : "branch";
        intptr_t status = _spawnl(_P_WAIT, argv[0], argv[0], argument, NULL);
        CHECK(status == 3); /* CRT abort, neither fallthrough nor access violation. */
        FILE *log = fopen("recomp_stop_stderr.txt", "r");
        char text[1024];
        CHECK(log);
        size_t length = fread(text, 1, sizeof text - 1, log);
        text[length] = 0;
        fclose(log);
        CHECK(strstr(text, mode ? "[STUB] unresolved 0x00123456" : "[DEAD-BRANCH] taken jump to 0x0009398F"));
        CHECK(strstr(text, "stopping"));
    }
    remove("recomp_stop_stderr.txt");
    puts("PASS: untaken branch falls through; residual branch and unresolved stub abort safely before fake execution");
    return 0;
}
