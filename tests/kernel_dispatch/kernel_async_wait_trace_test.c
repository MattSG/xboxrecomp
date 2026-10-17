/* Exercise real wait bridges and HLE without game assets. */
#include "../../src/kernel/kernel_bridge.c"
#include <io.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s status=%08X host_error=%lu\n", __LINE__, #c, g_eax, GetLastError()); fflush(stdout); exit(1); } } while (0)
recomp_func_t recomp_lookup(uint32_t address) { (void)address; abort(); }
recomp_func_t recomp_lookup_manual(uint32_t address) { (void)address; abort(); }

static uint8_t *memory;
static unsigned occurrences(const char *text, const char *needle) {
    unsigned n = 0;
    while ((text = strstr(text, needle)) != NULL) { ++n; text += strlen(needle); }
    return n;
}

static void invoke(unsigned ordinal, uint32_t object, uint32_t expected, DWORD expected_error) {
    uint32_t *stack = (uint32_t *)(memory + 0x20000);
    uint8_t before[0x200];
    g_esp = 0x20000; g_eax = 0xBAD;
    g_ecx = 0x11223344; g_edx = 0x22334455; g_ebx = 0x33445566;
    g_esi = 0x30000; g_edi = 0x44556677; g_ebp = 0x55667788;
    g_seh_ebp = 0x66778899; g_df = 1; g_fp_top = 3; g_fp_cmp = -1;
    g_fp_control_word = 0x37F; g_fp_cc = 0x4100;
    for (unsigned i = 0; i < 8; ++i) g_fp_stack[i] = i * 1.25 - 3.0;
    memset(stack, 0, 0x200);
    stack[0] = 0x0035C7F1; stack[1] = object;
    stack[ordinal == 159 ? 5 : ordinal == 233 ? 3 : 4] = 0x28000;
    memcpy(before, stack, sizeof before);
    SetLastError(0x1234);
    recomp_func_t fn = recomp_lookup_kernel(KERNEL_VA_BASE + (ordinal == 159 ? 0 : ordinal == 233 ? 4 : 8));
    CHECK(fn != NULL);
    fn();
    if (g_eax != expected || GetLastError() != expected_error) {
        printf("wait ordinal=%u object=%08X expected=%08X error=%lu actual=%08X error=%lu\n",
               ordinal, object, expected, expected_error, g_eax, GetLastError());
        fflush(stdout);
    }
    CHECK(g_eax == expected && GetLastError() == expected_error);
    CHECK(g_esp == 0x20004 + (ordinal == 159 ? 20 : ordinal == 233 ? 12 : 16));
    CHECK(g_xbox_kernel_caller == 0x0035C7F1);
    CHECK(g_ecx == 0x11223344 && g_edx == 0x22334455);
    CHECK(g_ebx == 0x33445566 && g_esi == 0x30000 && g_edi == 0x44556677);
    CHECK(g_ebp == 0x55667788 && g_seh_ebp == 0x66778899 && g_df == 1);
    CHECK(g_fp_top == 3 && g_fp_cmp == -1 && g_fp_control_word == 0x37F && g_fp_cc == 0x4100);
    for (unsigned i = 0; i < 8; ++i) CHECK(g_fp_stack[i] == i * 1.25 - 3.0);
    CHECK(memcmp(before, stack, sizeof before) == 0);
}

int main(int argc, char **argv) {
    int inline_events = argc > 1 && strcmp(argv[1], "inline") == 0;
    int tracing = argc > 1 && strcmp(argv[1], "off") != 0;
    FILE *captured;
    int saved;
    char *output = malloc(400000);
    HANDLE signaled, unsignaled;
    uint32_t on_token, off_token;
    DWORD native_error;
    NTSTATUS native_status;
    LARGE_INTEGER zero = {0};
    CHECK(output != NULL);
    CHECK(_putenv_s("RECOMP_ASYNC_WAIT_TRACE", tracing ? "1" : "0") == 0);
    CHECK(_putenv_s("RECOMP_TITLE_KEVENTS", inline_events ? "1" : "0") == 0);
    CHECK(_putenv_s("RECOMP_KERNEL_LOG_BUDGET", "0") == 0);
    CHECK(_putenv_s("RECOMP_GUEST_LOCK", "0") == 0);
    memory = VirtualAlloc(NULL, 0x100000, MEM_RESERVE, PAGE_NOACCESS);
    CHECK(memory && VirtualAlloc(memory, 0x40000, MEM_COMMIT, PAGE_READWRITE) == memory);
    g_xbox_mem_offset = (ptrdiff_t)memory;
    g_xbox_map_size = 0x100000;
    for (unsigned i = 0; i < 3; ++i) {
        unsigned ordinal = i == 0 ? 159 : i == 1 ? 233 : 234;
        g_slot_ordinals[i] = ordinal;
        g_slot_bridges[i] = bridge_for_ordinal(ordinal);
        g_slot_arg_bytes[i] = stdcall_args_for_ordinal(ordinal);
    }
    signaled = CreateEventA(NULL, TRUE, TRUE, NULL);
    unsignaled = CreateEventA(NULL, TRUE, FALSE, NULL);
    CHECK(signaled && unsignaled);
    on_token = bridge_handle_token(signaled); off_token = bridge_handle_token(unsignaled);
    CHECK(on_token && off_token);
    BRIDGE_MEM32(0x30000) = 0x1020;
    BRIDGE_MEM32(0x30004) = 0x103;
    BRIDGE_MEM32(0x30008) = 0x0035C33A;
    BRIDGE_MEM32(0x3000C) = 0x22000;
    BRIDGE_MEM32(0x22000) = 0x00040000; /* title-built notification KEVENT */
    BRIDGE_MEM32(0x22004) = 1;
    fflush(stderr); captured = tmpfile(); saved = _dup(_fileno(stderr));
    CHECK(captured && saved >= 0 && _dup2(_fileno(captured), _fileno(stderr)) == 0);
    /* These diagnostic pointers are reserved but have no committed backing. */
    g_esp = 0x50000; g_eax = 0; SetLastError(0x9988);
    g_xbox_kernel_caller = 0;
    bridge_async_wait_log(159, 0x50000, NULL, "probe", 0, 0);
    CHECK(GetLastError() == 0x9988 && g_esp == 0x50000 && g_eax == 0);
    {
        int type;
        HANDLE guest_event = ke_guest_event(0x22000, &type);
        CHECK((guest_event != NULL) == inline_events);
        SetLastError(0x1234);
        native_status = xbox_KeWaitForSingleObject(inline_events ? guest_event : (HANDLE)(uintptr_t)0x22000,
                                                  0, 0, 0, &zero);
        native_error = GetLastError();
        invoke(159, 0x22000, (uint32_t)native_status, native_error);
        CHECK(BRIDGE_MEM32(0x22004) == 1);
    }
    for (unsigned kind = 0; kind < 3; ++kind) {
        unsigned ordinal = kind == 0 ? 159 : kind == 1 ? 233 : 234;
        SetLastError(0x1234);
        native_status = xbox_KeWaitForSingleObject(signaled, 0, 0, 0, &zero); native_error = GetLastError();
        CHECK(native_status == STATUS_SUCCESS);
        invoke(ordinal, on_token, (uint32_t)native_status, native_error);
        SetLastError(0x1234);
        native_status = xbox_KeWaitForSingleObject(unsignaled, 0, 0, 0, &zero); native_error = GetLastError();
        CHECK(native_status == STATUS_TIMEOUT);
        for (unsigned i = 0; i < 40; ++i) invoke(ordinal, off_token, (uint32_t)native_status, native_error);
        SetLastError(0x1234);
        native_status = xbox_KeWaitForSingleObject((HANDLE)(uintptr_t)0x50000000, 0, 0, 0, &zero);
        native_error = GetLastError();
        CHECK(native_status & 0x80000000u);
        for (unsigned i = 0; i < 70; ++i) invoke(ordinal, 0x50000000, (uint32_t)native_status, native_error);
    }
    fflush(stderr); CHECK(_dup2(saved, _fileno(stderr)) == 0); _close(saved);
    rewind(captured);
    size_t length = fread(output, 1, 399999, captured); output[length] = 0; fclose(captured);
    /* Polling uses its own budget; later failures still produce 64 records. */
    CHECK(occurrences(output, "[ASYNC_WAIT]") == (tracing ? 3 * (32 + 64) : 0));
    if (tracing) {
        CHECK(strstr(output, "caller=0035C7F1"));
        CHECK(strstr(output, "request_callback=0035C33A request_context=00022000"));
        CHECK(strstr(output, "status=00000102"));
        CHECK(strstr(output, "requested=00050000") && strstr(output, "header_read=0"));
        CHECK(strstr(output, "request_read=1 request_status=00000103"));
        CHECK(strstr(output, "stack_read=0"));
        CHECK(strstr(output, "kind=unknown_object"));
        if (inline_events) CHECK(strstr(output, "kind=inline_kevent"));
    }
    CloseHandle(signaled); CloseHandle(unsignaled);
    VirtualFree(memory, 0, MEM_RELEASE); free(output);
    printf("PASS: async wait trace %s preserves real wait status, registers, memory and host error; separate bounded polling/failure budgets\n", argv[1]);
    return 0;
}
