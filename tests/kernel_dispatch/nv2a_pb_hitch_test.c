/* Use deterministic clocks and a synthetic command stream: tracing must not
 * change decoding, callback order or guest command bytes. No GPU is started. */
#include "kernel/kernel.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int64_t test_wall, test_cpu;
static unsigned clock_calls, kick_calls;
static uint8_t *memory;
static uint32_t mutate_payload_va;
static struct { uint32_t subch, method, param; } commands[64];
static unsigned command_count;
static unsigned snapshot_calls;

static BOOL WINAPI test_qpc(LARGE_INTEGER *value)
{
    clock_calls++;
    value->QuadPart = test_wall;
    return TRUE;
}

static BOOL WINAPI test_frequency(LARGE_INTEGER *value)
{
    value->QuadPart = 1000000;
    return TRUE;
}

static BOOL WINAPI test_thread_times(HANDLE thread, LPFILETIME created,
    LPFILETIME exited, LPFILETIME kernel, LPFILETIME user)
{
    (void)thread;
    memset(created, 0, sizeof *created);
    memset(exited, 0, sizeof *exited);
    memset(kernel, 0, sizeof *kernel);
    user->dwLowDateTime = (DWORD)test_cpu;
    user->dwHighDateTime = (DWORD)(test_cpu >> 32);
    return TRUE;
}

#define QueryPerformanceCounter test_qpc
#define QueryPerformanceFrequency test_frequency
#define GetThreadTimes test_thread_times
#include "../../src/kernel/nv2a_pb_scan.c"
#undef QueryPerformanceCounter
#undef QueryPerformanceFrequency
#undef GetThreadTimes

ptrdiff_t xbox_GetMemoryOffset(void)
{
    return (ptrdiff_t)memory - (ptrdiff_t)XBOX_CONTIG_BASE;
}

void xbox_Nv2aCaptureSnapshot(XBOX_NV2A_CAPTURE_STATE *out)
{
    uint32_t low = XBOX_CONTIG_BASE + 0x100000;
    uint32_t high = low + 4u * 1024u * 1024u - 4u;
    snapshot_calls++;
    memset(out, 0, sizeof *out);
    out->version = 1; out->bytes = sizeof *out; out->mirror_count = 1;
    out->user_put = snapshot_calls; out->user_get = 7;
    out->mirrors[0].device_bytes = XBOX_NV2A_CAPTURE_DEVICE_BYTES;
    memcpy(out->mirrors[0].device + 0x24, &low, sizeof low);
    memcpy(out->mirrors[0].device + 0x28, &high, sizeof high);
}

void nv2a_d3d_kick(void) { kick_calls++; }
void nv2a_pb_exec_report(void) {}
static uint32_t sema_last, sema_releases;
uint32_t nv2a_pb_exec_semaphore_last_value(void) { return sema_last; }
uint32_t nv2a_pb_exec_semaphore_releases(void) { return sema_releases; }
void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param)
{
    if (command_count >= 64) ExitProcess(30);
    commands[command_count].subch = subch;
    commands[command_count].method = method;
    commands[command_count++].param = param;
    if (mutate_payload_va && method == 0x1720 && param == 11) {
        *(uint32_t *)(memory + mutate_payload_va) = 99;
        mutate_payload_va = 0;
        SetLastError(0x1357);
    }
    if (method == 0x17FC && !param) {
        test_wall += 80000; /* A slow draw, independent of host scheduling. */
        test_cpu += 400000; /* 40 ms of CPU within 80 ms of elapsed time. */
    }
    if (method == 0x0130) test_wall += 16000; /* Normal flip pacing. */
    if (method == 0x1D70) { sema_last = param; sema_releases++; }
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

int main(int argc, char **argv)
{
    uint32_t *root, *called, *jumped;
    uint32_t original[12];
    int trace = argc > 1 && strcmp(argv[1], "trace") == 0;
    int capture = argc > 1 && strcmp(argv[1], "capture") == 0;
    CHECK(!capture || argc == 3);
    _putenv_s("RECOMP_PB_EXEC", "1");
    _putenv_s("RECOMP_PB_SCAN", "");
    _putenv_s("RECOMP_HITCH_TRACE", trace ? "1" : "0");
    _putenv_s("RECOMP_PB_CAPTURE", capture ? argv[2] : "");
    memory = calloc(1, XBOX_CONTIG_SIZE);
    CHECK(memory);
    root = (uint32_t *)(memory + 0x100);
    called = (uint32_t *)(memory + 0x1000);
    jumped = (uint32_t *)(memory + 0x2000);
    root[0] = (4u << 18) | (2u << 13) | 0x1720;
    for (unsigned i = 0; i < 4; i++) root[i + 1] = 10 + i;
    root[5] = 0x40000000 | (4u << 18) | (3u << 13) | 0x1800;
    for (unsigned i = 0; i < 4; i++) root[i + 6] = 20 + i;
    root[10] = 0x1002; /* CALL */
    root[11] = 0x2001; /* JUMP after RETURN */
    called[0] = (1u << 18) | 0x17FC;
    called[1] = 0;
    called[2] = 0x00020000; /* RETURN */
    /* FLIP_STALL at the end of an increasing packet must still count as
     * pacing, rather than hide the slow draw in the non-flip maximum. */
    jumped[0] = (5u << 18) | 0x0120;
    for (unsigned i = 1; i <= 5; i++) jumped[i] = 0;
    memcpy(original, root, sizeof original);
    test_wall = 1000000;
    nv2a_pb_scan(XBOX_CONTIG_BASE + 0x100, XBOX_CONTIG_BASE + 0x2018);
    CHECK(kick_calls == 1 && command_count == 14);
    CHECK(s_tot_calls == 1 && s_tot_jumps == 1 && !s_tot_unknown);
    for (unsigned i = 0; i < 4; i++) {
        CHECK(commands[i].subch == 2 && commands[i].method == 0x1720 + i * 4);
        CHECK(commands[i].param == 10 + i);
        CHECK(commands[i + 4].subch == 3 && commands[i + 4].method == 0x1800);
        CHECK(commands[i + 4].param == 20 + i);
    }
    CHECK(commands[8].method == 0x17FC && !commands[8].param);
    for (unsigned i = 0; i < 5; i++)
        CHECK(commands[9 + i].method == 0x0120 + i * 4 && !commands[9 + i].param);
    CHECK(memcmp(original, root, sizeof original) == 0);
    CHECK(trace ? clock_calls != 0 : clock_calls == 0);
    CHECK(capture || snapshot_calls == 0);

    /* PUT can stop after a header or in its payload. RETURN/CALL/JUMP-looking
     * parameter values must remain data when the next submission arrives. */
    {
        uint32_t *split = (uint32_t *)(memory + 0x400);
        uint32_t saved[5];
        command_count = 0;
        split[0] = (4u << 18) | (2u << 13) | 0x1720;
        split[1] = 0x00020000; split[2] = 0x1002;
        split[3] = 0x2001; split[4] = 0x11223344;
        memcpy(saved, split, sizeof saved);
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x400, XBOX_CONTIG_BASE + 0x404);
        CHECK(command_count == 0);
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x404, XBOX_CONTIG_BASE + 0x408);
        CHECK(command_count == 1 && commands[0].param == 0x00020000);
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x408, XBOX_CONTIG_BASE + 0x410);
        CHECK(command_count == 3);
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x410, XBOX_CONTIG_BASE + 0x414);
        CHECK(command_count == 4);
        for (unsigned i = 0; i < 4; i++) {
            CHECK(commands[i].subch == 2 && commands[i].method == 0x1720 + i * 4);
            CHECK(commands[i].param == saved[i + 1]);
        }
        CHECK(memcmp(saved, split, sizeof saved) == 0);

        command_count = 0;
        split[0] = 0x40000000 | (3u << 18) | (3u << 13) | 0x1800;
        split[1] = 0x2001; split[2] = 0x1002; split[3] = 0x00020000;
        split[4] = (1u << 18) | 0x1D8C; split[5] = 32;
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x400, XBOX_CONTIG_BASE + 0x408);
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x408, XBOX_CONTIG_BASE + 0x418);
        CHECK(command_count == 4);
        for (unsigned i = 0; i < 3; i++) {
            CHECK(commands[i].subch == 3 && commands[i].method == 0x1800);
            CHECK(commands[i].param == split[i + 1]);
        }
        CHECK(commands[3].method == 0x1D8C && commands[3].param == 32);
    }

    /* Called lists reach RETURN even when their caller's PUT immediately
     * follows CALL. The caller can then begin a split packet of its own. */
    {
        uint32_t *split = (uint32_t *)(memory + 0x600);
        command_count = 0;
        split[0] = 0x1002;
        split[1] = (3u << 18) | 0x1D8C;
        split[2] = 1; split[3] = 2; split[4] = 3;
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x600, XBOX_CONTIG_BASE + 0x604);
        CHECK(command_count == 1 && commands[0].method == 0x17FC);
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x604, XBOX_CONTIG_BASE + 0x608);
        CHECK(command_count == 1);
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x608, XBOX_CONTIG_BASE + 0x614);
        CHECK(command_count == 4);
        for (unsigned i = 0; i < 3; i++)
            CHECK(commands[i + 1].method == 0x1D8C + i * 4 && commands[i + 1].param == i + 1);
    }

    /* Complete a partial packet, follow an actual backwards ring JUMP,
     * and suspend/resume the next packet in the low part of the ring. */
    {
        uint32_t *high = (uint32_t *)(memory + 0x2F00);
        uint32_t *low = (uint32_t *)(memory + 0x800);
        command_count = 0;
        high[0] = (2u << 18) | 0x1720; high[1] = 0x11; high[2] = 0x22;
        high[3] = 0x801;
        low[0] = (2u << 18) | 0x1800; low[1] = 0x33; low[2] = 0x44;
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x2F00, XBOX_CONTIG_BASE + 0x2F08);
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x2F08, XBOX_CONTIG_BASE + 0x804);
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x804, XBOX_CONTIG_BASE + 0x80C);
        CHECK(command_count == 4);
        CHECK(commands[0].method == 0x1720 && commands[0].param == 0x11);
        CHECK(commands[1].method == 0x1724 && commands[1].param == 0x22);
        CHECK(commands[2].method == 0x1800 && commands[2].param == 0x33);
        CHECK(commands[3].method == 0x1804 && commands[3].param == 0x44);
    }

    /* Nothing wraps at 64 MB: a read past the window, or a JUMP/CALL target
     * outside it, stops the walk at PUT instead of executing whatever lies at
     * the wrapped address (stop-and-salvage, as apexJCL's walker). */
    {
        uint32_t *last = (uint32_t *)(memory + XBOX_CONTIG_SIZE - 4);
        uint32_t *first = (uint32_t *)memory;
        uint32_t stops = s_tot_stops;
        command_count = 0;

        /* A payload that would run off the top of RAM: its in-window
         * parameter runs, nothing is read from physical 0. */
        last[-1] = (3u << 18) | 0x1720; last[0] = 42;
        first[0] = 0x1002; first[1] = 0x2001;
        nv2a_pb_scan(XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - 8, XBOX_CONTIG_BASE + 8);
        CHECK(command_count == 1 && commands[0].method == 0x1720 && commands[0].param == 42);
        CHECK(s_tot_stops == stops + 1);
        CHECK(s_fifo.pc == XBOX_CONTIG_BASE + 8 && !s_packet.active);

        /* A JUMP above 64 MB stops; the commands at the wrapped address
         * do not run. */
        command_count = 0;
        first[0] = (1u << 18) | 0x17FC; first[1] = 0;
        root = (uint32_t *)(memory + 0x900);
        root[0] = (XBOX_CONTIG_SIZE + 0) | 1;
        root[1] = (1u << 18) | 0x1720; root[2] = 7;
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x900, XBOX_CONTIG_BASE + 0x90C);
        CHECK(command_count == 0 && s_tot_stops == stops + 2);

        /* A CALL above 64 MB stops the same way and leaves no call open. */
        root[0] = (XBOX_CONTIG_SIZE + 0) | 2;
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x900, XBOX_CONTIG_BASE + 0x90C);
        CHECK(command_count == 0 && s_tot_stops == stops + 3 && !s_fifo.in_call);

        /* After a stop, the next fence release between it and PUT still
         * runs (odd, after the last, within 128 of it), so the title's
         * fence wait completes. A release that doesn't look like the next
         * fence is left alone. */
        sema_last = 5;
        root[0] = (XBOX_CONTIG_SIZE + 0) | 1;
        root[1] = 0x00041D70; root[2] = 7;
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x900, XBOX_CONTIG_BASE + 0x90C);
        CHECK(command_count == 1 && commands[0].method == 0x1D70 && commands[0].param == 7);
        CHECK(s_tot_salvaged == 1);
        command_count = 0;
        root[2] = 0x9000;                  /* even and far ahead: data */
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x900, XBOX_CONTIG_BASE + 0x90C);
        CHECK(command_count == 0 && s_tot_salvaged == 1);
    }

    /* An explicitly discontinuous scan cannot inherit stale packet state. */
    {
        uint32_t *old = (uint32_t *)(memory + 0xC00);
        uint32_t *fresh = (uint32_t *)(memory + 0xD00);
        command_count = 0;
        old[0] = (3u << 18) | 0x1800;
        fresh[0] = (1u << 18) | 0x1720; fresh[1] = 42;
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0xC00, XBOX_CONTIG_BASE + 0xC04);
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0xD00, XBOX_CONTIG_BASE + 0xD08);
        CHECK(command_count == 1 && commands[0].method == 0x1720 && commands[0].param == 42);
    }

    /* PUT bounds the actual FIFO even within a CALL. Its payload and saved
     * return address survive publication; the caller then jumps around the
     * list instead of decoding that list a second time. */
    {
        uint32_t *caller = (uint32_t *)(memory + 0x5000);
        uint32_t *list = (uint32_t *)(memory + 0x5200);
        uint32_t *tail = (uint32_t *)(memory + 0x5300);
        uint32_t saved[5];
        command_count = 0;
        caller[0] = 0x5202; caller[1] = 0x5301;
        list[0] = (3u << 18) | 0x1720;
        list[1] = 0x00020000; list[2] = 0x1002; list[3] = 0x2001;
        list[4] = 0x00020000;
        tail[0] = (1u << 18) | 0x1D8C; tail[1] = 77;
        memcpy(saved, list, sizeof saved);
        CHECK(nv2a_pb_scan(XBOX_CONTIG_BASE + 0x5000, XBOX_CONTIG_BASE + 0x5208));
        CHECK(command_count == 1 && commands[0].param == 0x00020000);
        CHECK(s_fifo.in_call && s_packet.active && s_packet.remaining == 2);
        /* Above the window is refused, not folded back onto 0x5208: nothing
         * runs and the saved call and packet are untouched. */
        CHECK(nv2a_pb_scan(XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE + 0x5208,
                           XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE + 0x5308));
        CHECK(command_count == 1);
        CHECK(s_fifo.in_call && s_packet.active && s_packet.remaining == 2);
        CHECK(nv2a_pb_scan(XBOX_CONTIG_BASE + 0x5208, XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE + 0x10));
        CHECK(command_count == 1 && s_fifo.in_call && s_packet.active);
        CHECK(nv2a_pb_scan(XBOX_CONTIG_BASE + 0x5208, XBOX_CONTIG_BASE + 0x5308));
        CHECK(command_count == 4 && !s_fifo.in_call && !s_packet.active);
        for (unsigned i = 0; i < 3; ++i)
            CHECK(commands[i].method == 0x1720 + i * 4 && commands[i].param == saved[i + 1]);
        CHECK(commands[3].method == 0x1D8C && commands[3].param == 77);
        CHECK(!memcmp(saved, list, sizeof saved));

        /* A CALL can land directly on PUT, before any header is published. */
        command_count = 0;
        CHECK(nv2a_pb_scan(XBOX_CONTIG_BASE + 0x5000, XBOX_CONTIG_BASE + 0x5200));
        CHECK(command_count == 0 && s_fifo.in_call && !s_packet.active);
        CHECK(nv2a_pb_scan(XBOX_CONTIG_BASE + 0x5200, XBOX_CONTIG_BASE + 0x5308));
        CHECK(command_count == 4 && !s_fifo.in_call);
    }

    /* A called payload that runs off the top of RAM stops there too: the
     * in-window parameter runs, the call is dropped, and the walk resumes
     * at PUT rather than reading physical 0. */
    {
        uint32_t *last = (uint32_t *)(memory + XBOX_CONTIG_SIZE - 4);
        uint32_t *first = (uint32_t *)memory;
        uint32_t *caller = (uint32_t *)(memory + 0x9000);
        uint32_t *tail = (uint32_t *)(memory + 0x9400);
        uint32_t stops = s_tot_stops;
        command_count = 0;
        caller[0] = (XBOX_CONTIG_SIZE - 8) | 2; caller[1] = 0x9401;
        last[-1] = (3u << 18) | 0x1720; last[0] = 11;
        first[0] = 22; first[1] = 33; first[2] = 0x00020000;
        tail[0] = (1u << 18) | 0x1D8C; tail[1] = 44;
        CHECK(nv2a_pb_scan(XBOX_CONTIG_BASE + 0x9000, XBOX_CONTIG_BASE + 0x9408));
        CHECK(command_count == 1 && commands[0].param == 11);
        CHECK(s_tot_stops == stops + 1 && !s_fifo.in_call && !s_packet.active);
    }

    /* RETURN is the exact00020000 word; reserved high bits cannot create a
     * fake method packet and consume the next valid header as a parameter. */
    {
        uint32_t *caller = (uint32_t *)(memory + 0xA000);
        uint32_t *list = (uint32_t *)(memory + 0xB000);
        uint32_t before_unknown = s_tot_unknown;
        command_count = 0;
        caller[0] = 0xB002;
        list[0] = 0x00020004;
        list[1] = 0x60000000 | (1u << 18) | 0x1720;
        list[2] = (1u << 18) | 0x1D8C; list[3] = 55;
        list[4] = 0x00020000;
        CHECK(nv2a_pb_scan(XBOX_CONTIG_BASE + 0xA000, XBOX_CONTIG_BASE + 0xA004));
        CHECK(command_count == 1 && commands[0].method == 0x1D8C && commands[0].param == 55);
        CHECK(s_tot_unknown - before_unknown == 2 && !s_fifo.in_call);
    }

    /* Increasing method addresses retain the register's11-bit field width,
     * including a payload split at the wrap. */
    {
        uint32_t *split = (uint32_t *)(memory + 0xC100);
        command_count = 0;
        split[0] = (3u << 18) | 0x1FFC; split[1] = 10;
        split[2] = 20; split[3] = 30;
        CHECK(nv2a_pb_scan(XBOX_CONTIG_BASE + 0xC100, XBOX_CONTIG_BASE + 0xC108));
        CHECK(nv2a_pb_scan(XBOX_CONTIG_BASE + 0xC108, XBOX_CONTIG_BASE + 0xC110));
        CHECK(command_count == 3);
        CHECK(commands[0].method == 0x1FFC && commands[0].param == 10);
        CHECK(commands[1].method == 0 && commands[1].param == 20);
        CHECK(commands[2].method == 4 && commands[2].param == 30);
    }

    /* A scheduling cap is incomplete, including inside a payload. Retrying
     * the original start with a newer PUT executes the saved remainder once
     * and does not skip the additional publication. */
    {
        uint32_t *stream = (uint32_t *)(memory + 0x100000);
        uint32_t start = XBOX_CONTIG_BASE + 0x100000;
        uint32_t old_end = start + (0x100000u + 2) * 4;
        uint32_t before_words = s_tot_words;
        command_count = 0;
        memset(stream, 0, (0x100000u + 4) * 4);
        stream[0x100000u - 2] = (3u << 18) | 0x1720;
        stream[0x100000u - 1] = 11;
        stream[0x100000u] = 22; stream[0x100000u + 1] = 33;
        stream[0x100000u + 2] = (1u << 18) | 0x1D8C;
        stream[0x100000u + 3] = 44;
        if (capture) mutate_payload_va = 0x100000 + (0x100000u - 1) * 4;
        CHECK(!nv2a_pb_scan(start, old_end));
        CHECK(s_tot_words - before_words == 0x100000u);
        CHECK(command_count == 1 && s_packet.active && s_packet.remaining == 2);
        if (capture) {
            const char *suffixes[] = { ".ram.bin", ".reads.bin", ".recent.bin", ".json",
                ".initial-ring.bin", ".state-initial.bin", ".state-final.bin", ".producer.json" };
            char path[4096], metadata[4096];
            PB_CAPTURE_READ recorded;
            XBOX_NV2A_CAPTURE_STATE state;
            uint32_t copied;
            FILE *file;
            CHECK(GetLastError() == 0x1357);
            CHECK(s_capture.done && !s_capture.first && !s_capture.ring && !s_capture.recording);
            snprintf(path, sizeof path, "%s.reads.bin", argv[2]);
            file = fopen(path, "rb"); CHECK(file);
            CHECK(!fseek(file, 0, SEEK_END));
            CHECK(ftell(file) == (long)(0x100000u * sizeof recorded));
            CHECK(!fseek(file, -(long)sizeof recorded, SEEK_END));
            CHECK(fread(&recorded, sizeof recorded, 1, file) == 1);
            CHECK(!fclose(file));
            snprintf(path, sizeof path, "%s.initial-ring.bin", argv[2]);
            file = fopen(path, "rb"); CHECK(file);
            CHECK(!fseek(file, 0, SEEK_END) && ftell(file) == 4u * 1024u * 1024u);
            CHECK(!fseek(file, -(long)sizeof copied, SEEK_END));
            CHECK(fread(&copied, sizeof copied, 1, file) == 1 && copied == 11);
            CHECK(!fclose(file));
            snprintf(path, sizeof path, "%s.state-initial.bin", argv[2]);
            file = fopen(path, "rb"); CHECK(file);
            CHECK(fread(&state, sizeof state, 1, file) == 1);
            CHECK(!fclose(file) && state.user_put + 1 == snapshot_calls);
            snprintf(path, sizeof path, "%s.state-final.bin", argv[2]);
            file = fopen(path, "rb"); CHECK(file);
            CHECK(fread(&state, sizeof state, 1, file) == 1);
            CHECK(!fclose(file) && state.user_put == snapshot_calls);
            CHECK(recorded.va == start + (0x100000u - 1) * 4);
            CHECK(recorded.word == 11 && recorded.method == 0x1720);
            CHECK(recorded.remaining == 3 && recorded.flags == 1);
            snprintf(path, sizeof path, "%s.ram.bin", argv[2]);
            file = fopen(path, "rb"); CHECK(file);
            CHECK(!fseek(file, 0, SEEK_END) && ftell(file) == XBOX_CONTIG_SIZE);
            CHECK(!fseek(file, (long)(recorded.va - XBOX_CONTIG_BASE), SEEK_SET));
            CHECK(fread(&copied, sizeof copied, 1, file) == 1 && copied == 99);
            CHECK(!fclose(file));
            snprintf(path, sizeof path, "%s.recent.bin", argv[2]);
            file = fopen(path, "rb"); CHECK(file);
            CHECK(!fseek(file, -(long)sizeof recorded, SEEK_END));
            CHECK(fread(&recorded, sizeof recorded, 1, file) == 1 && recorded.word == 11);
            CHECK(!fclose(file));
            snprintf(path, sizeof path, "%s.json", argv[2]);
            file = fopen(path, "rb"); CHECK(file);
            size_t length = fread(metadata, 1, sizeof metadata - 1, file);
            metadata[length] = 0; CHECK(!fclose(file));
            CHECK(strstr(metadata, "\"first_reads\":1048576"));
            CHECK(strstr(metadata, "\"ram_ok\":1,\"first_ok\":1,\"recent_ok\":1"));
            CHECK(strstr(metadata, "\"initial_ring_bytes\":4194304,\"initial_ring_ok\":1"));
            CHECK(strstr(metadata, "\"initial_state_ok\":1,\"final_state_ok\":1,\"producer_ok\":1"));
            snprintf(path, sizeof path, "%s.producer.json", argv[2]);
            file = fopen(path, "rb"); CHECK(file);
            length = fread(metadata, 1, sizeof metadata - 1, file);
            metadata[length] = 0; CHECK(!fclose(file));
            CHECK(strstr(metadata, "\"initial\":{\"version\":1"));
            CHECK(strstr(metadata, "\"final\":{\"version\":1"));
            CHECK(strstr(metadata, "\"ring_high\":2152726524"));
            for (unsigned i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
                snprintf(path, sizeof path, "%s%s", argv[2], suffixes[i]);
                CHECK(!remove(path));
            }
        }
        CHECK(nv2a_pb_scan(start, old_end + 8));
        CHECK(command_count == 4 && !s_packet.active && !s_fifo.active);
        CHECK(commands[0].method == 0x1720 && commands[0].param == 11);
        CHECK(commands[1].method == 0x1724 && commands[1].param == 22);
        CHECK(commands[2].method == 0x1728 && commands[2].param == 33);
        CHECK(commands[3].method == 0x1D8C && commands[3].param == 44);
    }

    /* Called lists share the root's cap; they neither enlarge it nor lose the
     * saved return PC when the next ACK tick resumes their header. */
    {
        uint32_t *caller = (uint32_t *)(memory + 0xC5000);
        uint32_t *list = (uint32_t *)(memory + 0x800000);
        uint32_t start = XBOX_CONTIG_BASE + 0xC5000;
        uint32_t before_words = s_tot_words;
        command_count = 0;
        memset(list, 0, (0x100000u + 2) * 4);
        caller[0] = 0x800002; caller[1] = 0xC5011;
        caller[4] = (1u << 18) | 0x1D8C; caller[5] = 66;
        list[0x100000u - 1] = (1u << 18) | 0x1720;
        list[0x100000u] = 55; list[0x100000u + 1] = 0x00020000;
        CHECK(!nv2a_pb_scan(start, start + 24));
        CHECK(s_tot_words - before_words == 0x100000u);
        CHECK(command_count == 0 && s_fifo.in_call);
        CHECK(nv2a_pb_scan(start, start + 24));
        CHECK(command_count == 2 && !s_fifo.active && !s_fifo.in_call);
        CHECK(commands[0].method == 0x1720 && commands[0].param == 55);
        CHECK(commands[1].method == 0x1D8C && commands[1].param == 66);
    }

    /* A discontinuous replacement discards an unfinished called list as well
     * as its packet; it cannot return into a previous FIFO. */
    {
        uint32_t *caller = (uint32_t *)(memory + 0xE000);
        uint32_t *list = (uint32_t *)(memory + 0xE200);
        uint32_t *fresh = (uint32_t *)(memory + 0xE400);
        command_count = 0;
        caller[0] = 0xE202;
        list[0] = (3u << 18) | 0x1720; list[1] = 12;
        CHECK(nv2a_pb_scan(XBOX_CONTIG_BASE + 0xE000, XBOX_CONTIG_BASE + 0xE208));
        CHECK(command_count == 1 && s_fifo.in_call && s_packet.active);
        fresh[0] = (1u << 18) | 0x1D8C; fresh[1] = 34;
        CHECK(nv2a_pb_scan(XBOX_CONTIG_BASE + 0xE400, XBOX_CONTIG_BASE + 0xE408));
        CHECK(command_count == 2 && commands[1].method == 0x1D8C && commands[1].param == 34);
        CHECK(!s_fifo.in_call && !s_packet.active);
    }

    /* Empty packets execute nothing and need no per-packet diagnostic clock. */
    {
        unsigned before_clocks = clock_calls;
        command_count = 0;
        memset(memory + 0x1800, 0, 1000 * 4);
        nv2a_pb_scan(XBOX_CONTIG_BASE + 0x1800, XBOX_CONTIG_BASE + 0x27A0);
        CHECK(command_count == 0);
        CHECK(clock_calls - before_clocks == (trace ? 2u : 0u));
    }
    CHECK(trace ? clock_calls != 0 : clock_calls == 0);
    CHECK(capture ? snapshot_calls > 1 : snapshot_calls == 0);
    free(memory);
    puts("PASS: tracing, PUT/cap continuations, exact decoding and ring/bus wraps preserve command order");
    return 0;
}
