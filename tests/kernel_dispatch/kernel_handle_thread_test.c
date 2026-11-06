/* Real event/file imports share the bridge's actual private handle table. */
#include "../../src/kernel/kernel_bridge.c"
#include "../kernel_test_host.h"

#define WORKERS 12
#define ROUNDS 32
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "FAIL: line %d: %s (error %lu)\n", __LINE__, #c, GetLastError()); \
    exit(1); \
} } while (0)

recomp_func_t recomp_lookup(uint32_t address) { (void)address; abort(); }
recomp_func_t recomp_lookup_manual(uint32_t address) { (void)address; abort(); }

enum { CLOSE, CREATE_EVENT, CREATE_FILE, READ_FILE, SET_EVENT, WAIT_EVENT, WRITE_FILE, SERVICES };
static const uint32_t ordinals[SERVICES] = {187, 189, 190, 219, 225, 233, 236};
static uint32_t services[SERVICES];
static uint8_t *memory;
static uint32_t root_token;
static char directory[MAX_PATH];

typedef struct {
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE changed;
    unsigned count, generation;
} barrier_t;
static barrier_t barrier;

static void rendezvous(void)
{
    unsigned generation;
    EnterCriticalSection(&barrier.lock);
    generation = barrier.generation;
    if (++barrier.count == WORKERS + 1) {
        barrier.count = 0;
        barrier.generation++;
        WakeAllConditionVariable(&barrier.changed);
    } else {
        while (generation == barrier.generation)
            CHECK(SleepConditionVariableCS(&barrier.changed, &barrier.lock, 10000));
    }
    LeaveCriticalSection(&barrier.lock);
}

static uint32_t invoke(unsigned service, uint32_t stack, const uint32_t *args, unsigned count)
{
    recomp_func_t fn;
    uint32_t *sp = (uint32_t *)(memory + stack);
    sp[0] = 0xBEEF0001;
    memcpy(sp + 1, args, count * sizeof *args);
    g_esp = stack;
    fn = recomp_lookup_kernel(services[service]);
    CHECK(fn != NULL);
    fn();
    CHECK(g_esp == stack + 4 + count * 4);
    return g_eax;
}

static uint32_t arena(unsigned id) { return 0x20000 + id * 0x1000; }
static uint32_t guest_word(uint32_t va) { return *(uint32_t *)(memory + va); }

static void close_token(uint32_t token, uint32_t stack)
{
    CHECK(invoke(CLOSE, stack, &token, 1) == STATUS_SUCCESS);
}

static void file_name(unsigned id, unsigned round, char *name, size_t bytes)
{
    CHECK(snprintf(name, bytes, "worker-%02u-%02u.bin", id, round) > 0);
}

static void delete_file(unsigned id, unsigned round)
{
    char name[40], path[MAX_PATH];
    file_name(id, round, name, sizeof name);
    CHECK(xt_join(path, MAX_PATH, directory, name) > 0);
    CHECK(xt_delete_file(path));
}

static void open_pair(unsigned id, unsigned round, uint32_t *event_token, uint32_t *file_token)
{
    uint32_t base = arena(id);
    uint32_t event_args[] = {base + 0x100, 0, 0, 0};
    uint32_t file_args[] = {base + 0x104,
        XBOX_GENERIC_READ | XBOX_GENERIC_WRITE | XBOX_SYNCHRONIZE,
        base + 0x160, base + 0x120, 0, 0, 0, XBOX_FILE_CREATE,
        XBOX_FILE_SYNCHRONOUS_IO_NONALERT | XBOX_FILE_NON_DIRECTORY_FILE};
    uint32_t io_args[] = {0, 0, 0, 0, base + 0x120, base + 0x280, 32, base + 0x300};
    uint32_t *oa = (uint32_t *)(memory + base + 0x160);
    uint16_t *name = (uint16_t *)(memory + base + 0x190);
    uint32_t *payload = (uint32_t *)(memory + base + 0x280);
    unsigned j;
    char *filename = (char *)(memory + base + 0x200);
    file_name(id, round, filename, 64);
    memset(oa, 0, 12);
    oa[0] = root_token;
    oa[1] = base + 0x190;
    name[0] = (uint16_t)strlen(filename);
    name[1] = name[0] + 1;
    *(uint32_t *)(name + 2) = base + 0x200;
    *(uint64_t *)(memory + base + 0x300) = 0;
    CHECK(invoke(CREATE_EVENT, base, event_args, 4) == STATUS_SUCCESS);
    *event_token = guest_word(base + 0x100);
    CHECK(invoke(CREATE_FILE, base, file_args, 9) == STATUS_SUCCESS);
    *file_token = guest_word(base + 0x104);
    CHECK(*event_token && *file_token && *event_token != *file_token);
    for (j = 0; j < 8; ++j) payload[j] = 0xCA000000u | id << 16 | round << 8 | j;
    io_args[0] = *file_token;
    CHECK(invoke(WRITE_FILE, base, io_args, 8) == STATUS_SUCCESS);
    CHECK(guest_word(base + 0x124) == 32);
    io_args[5] = base + 0x2A0;
    memset(memory + base + 0x2A0, 0, 32);
    CHECK(invoke(READ_FILE, base, io_args, 8) == STATUS_SUCCESS);
    CHECK(guest_word(base + 0x124) == 32);
    CHECK(memcmp(memory + base + 0x280, memory + base + 0x2A0, 32) == 0);
}

static DWORD WINAPI io_worker(LPVOID parameter)
{
    unsigned id = (unsigned)(uintptr_t)parameter, round;
    uint32_t event_token = 0, file_token = 0;
    for (round = 0; round < ROUNDS; ++round) {
        uint32_t next_event, next_file;
        rendezvous();
        /* Half the workers create before closing their previous pair. The
         * other half close first, so registrations and takes overlap on
         * distinct live objects without racing an object's own native use.
         */
        if (round && (id & 1)) {
            close_token(event_token, arena(id));
            close_token(file_token, arena(id));
            delete_file(id, round - 1);
        }
        open_pair(id, round, &next_event, &next_file);
        if (round && !(id & 1)) {
            close_token(event_token, arena(id));
            close_token(file_token, arena(id));
            delete_file(id, round - 1);
        }
        event_token = next_event;
        file_token = next_file;
        rendezvous();
        rendezvous();
    }
    rendezvous();
    close_token(event_token, arena(id));
    close_token(file_token, arena(id));
    delete_file(id, ROUNDS - 1);
    rendezvous();
    return 0;
}

static void check_live_pairs(void)
{
    uint32_t tokens[WORKERS * 2];
    HANDLE handles[WORKERS * 2];
    unsigned i, j;
    for (i = 0; i < WORKERS * 2; ++i) {
        uint32_t va = arena(i / 2) + 0x100 + (i & 1) * 4;
        DWORD flags;
        tokens[i] = guest_word(va);
        handles[i] = bridge_read_handle(va);
        CHECK((tokens[i] & 0xFF000000u) == BRIDGE_HANDLE_TAG);
        CHECK(handles[i] && handles[i] == bridge_resolve_handle(tokens[i]));
        CHECK(GetHandleInformation(handles[i], &flags));
        for (j = 0; j < i; ++j)
            CHECK(tokens[i] != tokens[j] && handles[i] != handles[j]);
    }
    /* Signal one guest event at a time, and observe all events through real
     * wait imports. A token collision cannot pass this identity check.
     */
    *(uint64_t *)(memory + 0x100300) = 0;
    for (i = 0; i < WORKERS; ++i) {
        uint32_t set_args[] = {tokens[i * 2], 0};
        CHECK(invoke(SET_EVENT, 0x100000, set_args, 2) == STATUS_SUCCESS);
        for (j = 0; j < WORKERS; ++j) {
            uint32_t wait_args[] = {tokens[j * 2], 0, 0x100300};
            CHECK(invoke(WAIT_EVENT, 0x100000, wait_args, 3) ==
                  (j <= i ? STATUS_SUCCESS : STATUS_TIMEOUT));
        }
    }
}

static HANDLE shared_handle;
static uint32_t shared_token, results[WORKERS];
static HANDLE taken[WORKERS];
static int take_mode;
static DWORD WINAPI same_handle_worker(LPVOID parameter)
{
    unsigned id = (unsigned)(uintptr_t)parameter;
    rendezvous();
    if (take_mode) taken[id] = bridge_take_handle(shared_token);
    else results[id] = bridge_handle_token(shared_handle);
    rendezvous();
    return 0;
}

static void same_handle_cohort(int taking)
{
    HANDLE threads[WORKERS];
    unsigned i, winners = 0;
    take_mode = taking;
    for (i = 0; i < WORKERS; ++i) {
        threads[i] = CreateThread(NULL, 0, same_handle_worker, (LPVOID)(uintptr_t)i, 0, NULL);
        CHECK(threads[i]);
    }
    rendezvous();
    rendezvous();
    CHECK(WaitForMultipleObjects(WORKERS, threads, TRUE, 10000) == WAIT_OBJECT_0);
    for (i = 0; i < WORKERS; ++i) {
        if (taking) {
            CHECK(taken[i] == NULL || taken[i] == shared_handle);
            winners += taken[i] != NULL;
        } else CHECK(results[i] == shared_token);
        CHECK(CloseHandle(threads[i]));
    }
    if (taking) CHECK(winners == 1 && bridge_resolve_handle(shared_token) == NULL);
}

static void table_edges(void)
{
    uint32_t invalid[] = {BRIDGE_HANDLE_TAG, BRIDGE_HANDLE_TAG | BRIDGE_HANDLE_MAX,
                          BRIDGE_HANDLE_TAG | BRIDGE_HANDLE_MASK};
    HANDLE wide = (HANDLE)(uintptr_t)UINT64_C(0x123456780000);
    HANDLE overflow;
    DWORD flags;
    unsigned i;
    uint32_t token;
    CHECK(bridge_handle_token(NULL) == 0 && bridge_handle_token(INVALID_HANDLE_VALUE) == 0);
    token = bridge_handle_token(wide);
    CHECK(token == (BRIDGE_HANDLE_TAG | 1));
    CHECK(bridge_handle_token(wide) == token && bridge_resolve_handle(token) == wide);
    bridge_write_handle(0x18000, wide);
    CHECK(guest_word(0x18000) == token && bridge_read_handle(0x18000) == wide);
    CHECK(bridge_take_handle(token) == wide && bridge_take_handle(token) == NULL);
    CHECK(bridge_resolve_handle(0xFFFFFFFEu) == (HANDLE)(intptr_t)-2);
    CHECK(bridge_resolve_handle(0xFFFFFFFFu) == (HANDLE)(intptr_t)-1);
    /* Untagged values pass through only when the bridge issues them as
     * sentinels. 0xDEAD0001 and 0xBEEF0010 are no longer issued, and a synthetic
     * thunk VA is never a handle, so all three are refused. */
    CHECK(bridge_resolve_handle(0xDEAD0001u) == NULL);
    CHECK(bridge_resolve_handle(0xFE000000u) == NULL);
    CHECK(bridge_resolve_handle(0xBEEF0001u) == (HANDLE)(uintptr_t)0xBEEF0001u);
    CHECK(bridge_take_handle(0xDEAD0001u) == NULL);
    for (i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        *(uint32_t *)(memory + 0x18000) = invalid[i];
        CHECK(bridge_read_handle(0x18000) == NULL);
        CHECK(bridge_resolve_handle(invalid[i]) == NULL && bridge_take_handle(invalid[i]) == NULL);
    }
    *(uint32_t *)(memory + 0x18000) = 0xBEEF0010u;
    CHECK(bridge_read_handle(0x18000) == NULL);

    /* Populate exact private bounds without O(N^2) registrations. Exhaustion
     * must retain the caller's ownership and still deduplicate existing slots.
     */
    AcquireSRWLockExclusive(&s_handle_table_lock);
    for (i = 1; i < BRIDGE_HANDLE_MAX; ++i)
        bridge_handle_store(i, (HANDLE)((uintptr_t)wide + i * 16));
    ReleaseSRWLockExclusive(&s_handle_table_lock);
    overflow = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(overflow);
    CHECK(bridge_handle_token(overflow) == 0 && GetHandleInformation(overflow, &flags));
    CHECK(bridge_handle_token((HANDLE)((uintptr_t)wide + (BRIDGE_HANDLE_MAX - 1) * 16)) ==
          (BRIDGE_HANDLE_TAG | (BRIDGE_HANDLE_MAX - 1)));
    CHECK(bridge_take_handle(BRIDGE_HANDLE_TAG | 1) == (HANDLE)((uintptr_t)wide + 16));
    CHECK(bridge_handle_token(overflow) == (BRIDGE_HANDLE_TAG | 1));
    CHECK(bridge_take_handle(BRIDGE_HANDLE_TAG | 1) == overflow && CloseHandle(overflow));
    AcquireSRWLockExclusive(&s_handle_table_lock);
    for (i = 1; i < BRIDGE_HANDLE_MAX; ++i) bridge_handle_store(i, NULL);
    ReleaseSRWLockExclusive(&s_handle_table_lock);

    shared_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(shared_handle);
    shared_token = bridge_handle_token(shared_handle);
    same_handle_cohort(0);
    same_handle_cohort(1);
    CHECK(CloseHandle(shared_handle));
}

int main(void)
{
    /* This fixture owns fixed raw scratch addresses below the export page. */
    if (!xbox_ConfigureKernelData(0x00740000u)) return 90;

    HANDLE root, threads[WORKERS];
    DWORD count, live_count = 0;
    unsigned i, round;
    CHECK(_putenv_s("RECOMP_KERNEL_LOG_BUDGET", "0") == 0);
    CHECK(_putenv_s("RECOMP_GUEST_LOCK", "0") == 0);
    memory = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(memory);
    g_xbox_mem_offset = (ptrdiff_t)memory;
    InitializeCriticalSection(&barrier.lock);
    InitializeConditionVariable(&barrier.changed);
    for (i = 0; i < SERVICES; ++i) *(uint32_t *)(memory + 0x10000 + i * 4) = 0x80000000u | ordinals[i];
    xbox_kernel_set_thunk_address(0x10000, SERVICES);
    xbox_kernel_bridge_init();
    for (i = 0; i < SERVICES; ++i) services[i] = guest_word(0x10000 + i * 4);
    table_edges();
    CHECK(xt_make_temp_dir(directory, sizeof directory));
    root = CreateFileA(directory, FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    CHECK(root != INVALID_HANDLE_VALUE);
    root_token = bridge_handle_token(root);
    for (i = 0; i < WORKERS; ++i) {
        threads[i] = CreateThread(NULL, 0, io_worker, (LPVOID)(uintptr_t)i, 0, NULL);
        CHECK(threads[i]);
    }
    for (round = 0; round < ROUNDS; ++round) {
        rendezvous();
        rendezvous();
        check_live_pairs();
#ifdef _WIN32   /* POSIX events are not descriptors; see kernel_test_host.h */
        CHECK(GetProcessHandleCount(GetCurrentProcess(), &count));
        if (!round) live_count = count;
        CHECK(count == live_count);
#endif
        rendezvous();
    }
    rendezvous();
    rendezvous();
#ifdef _WIN32
    CHECK(GetProcessHandleCount(GetCurrentProcess(), &count) && count + WORKERS * 2 == live_count);
#endif
    CHECK(WaitForMultipleObjects(WORKERS, threads, TRUE, 10000) == WAIT_OBJECT_0);
    for (i = 0; i < WORKERS; ++i) CHECK(CloseHandle(threads[i]));
    CHECK(bridge_take_handle(root_token) == root && CloseHandle(root));
    for (i = 1; i < BRIDGE_HANDLE_MAX; ++i) CHECK(bridge_handle_load(i) == NULL);
    CHECK(xt_remove_dir(directory));
    DeleteCriticalSection(&barrier.lock);
    CHECK(VirtualFree(memory, 0, MEM_RELEASE));
    puts("PASS: concurrent event/file imports retain unique tokens, exact identities/data and no native-handle leaks");
    return 0;
}
