/* Exercise the real dispatcher across guest threads with no game assets. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "../kernel_test_host.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*guest_fn)(void);
extern guest_fn recomp_lookup_kernel(uint32_t);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

static uint8_t *memory;
static uint32_t service;

static void dispatch(guest_fn fn, uint32_t stack)
{
    g_esp = stack;
    *(uint32_t *)(memory + stack) = 0xBEEF0001;
    fn();
    if (g_esp != stack + 4 || g_eax != 0)
        abort();
}

static DWORD WINAPI worker(LPVOID stack)
{
    guest_fn fn = recomp_lookup_kernel(service);
    unsigned i;
    if (!fn)
        return 1;
    for (i = 0; i < 10000; ++i)
        dispatch(fn, (uint32_t)(uintptr_t)stack);
    return 0;
}

static unsigned occurrences(const char *text, const char *needle)
{
    unsigned count = 0;
    while ((text = strstr(text, needle)) != NULL) {
        count++;
        text += strlen(needle);
    }
    return count;
}

int main(int argc, char **argv)
{
    /* This fixture owns fixed raw scratch addresses below the export page. */
    if (!xbox_ConfigureKernelData(0x00740000u)) return 90;

    int summary = argc == 2 && strcmp(argv[1], "summary") == 0;
    int budget = argc == 2 && strcmp(argv[1], "budget") == 0;
    FILE *captured;
    int saved_stderr;
    HANDLE threads[2];
    char output[4096];
    size_t length;
    unsigned i;
    guest_fn fn;
    if (_putenv_s("RECOMP_KERNEL_LOG_BUDGET", budget ? "3" : "0") ||
        _putenv_s("RECOMP_KERNEL_SUMMARY", summary ? "1" : "") ||
        _putenv_s("RECOMP_GUEST_LOCK", "0"))
        return 10;
    memory = VirtualAlloc(NULL, 16 * 1024 * 1024,
                          MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!memory)
        return 11;
    g_xbox_mem_offset = (ptrdiff_t)memory;
    *(uint32_t *)(memory + 0x10000) = 0x80000000u | 103;
    xbox_kernel_set_thunk_address(0x10000, 1);
    xbox_kernel_bridge_init();
    service = *(uint32_t *)(memory + 0x10000);

    fflush(stderr);
    captured = tmpfile();
    saved_stderr = _dup(_fileno(stderr));
    if (!captured || saved_stderr < 0 ||
        _dup2(_fileno(captured), _fileno(stderr)) != 0)
        return 12;
    for (i = 0; i < 2; ++i) {
        threads[i] = CreateThread(NULL, 0, worker,
                                  (LPVOID)(uintptr_t)(0x20000 + i * 0x1000), 0, NULL);
        if (!threads[i])
            return 13;
    }
    if (WaitForMultipleObjects(2, threads, TRUE, 5000) != WAIT_OBJECT_0)
        return 14;
    for (i = 0; i < 2; ++i) {
        DWORD result = 1;
        GetExitCodeThread(threads[i], &result);
        CloseHandle(threads[i]);
        if (result)
            return 15;
    }
    /* Pass the old mandatory-summary threshold, then dispatch once more. */
    Sleep(2100);
    fn = recomp_lookup_kernel(service);
    if (!fn)
        return 16;
    dispatch(fn, 0x22000);
    fflush(stderr);
    _dup2(saved_stderr, _fileno(stderr));
    _close(saved_stderr);
    rewind(captured);
    length = fread(output, 1, sizeof output - 1, captured);
    output[length] = '\0';
    fclose(captured);
    VirtualFree(memory, 0, MEM_RELEASE);

    if (occurrences(output, "[KERNEL] #") != (budget ? 3u : 0u) ||
        occurrences(output, "[KERNEL] summary:") != (summary ? 1u : 0u) ||
        (budget && (occurrences(output, "[KERNEL] #1:") != 1 ||
                    occurrences(output, "[KERNEL] #2:") != 1 ||
                    occurrences(output, "[KERNEL] #3:") != 1)) ||
        (summary && (!strstr(output, "20001 total calls") ||
                     !strstr(output, "ordinal 103 x20001")))) {
        fprintf(stderr, "FAIL: kernel log mode %s:\n%s", argc > 1 ? argv[1] : "quiet", output);
        return 1;
    }
    printf("PASS: kernel log mode %s preserves quiet gameplay and explicit diagnostics\n",
           argc > 1 ? argv[1] : "quiet");
    return 0;
}
