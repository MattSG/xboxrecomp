/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#include <stdio.h>
#include "platform/recomp_profile.h"
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "xbox_memory_layout.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
static uint32_t s_tot_calls;
/* Walks that stopped short of PUT, and how many of those had their fence
 * release salvaged. Counted because after eight log lines a stop is silent. */
static uint32_t s_tot_stops, s_tot_salvaged;
/* Segments refused at entry because GET or PUT is outside the window. */
static uint32_t s_tot_refused;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
extern uint32_t nv2a_pb_exec_semaphore_last_value(void);
extern uint32_t nv2a_pb_exec_semaphore_releases(void);
static int s_exec_enabled = -1;
static int s_scan_enabled = -1;

/* Clears the NV2A request bits (xbox_memory_layout.c); called every 256
 * words while executing, so kickoffs are answered mid-walk. */
extern void xbox_Nv2aAckBusyBits(void);
static uint32_t s_ack_tick;

/* Either switch, read once. The answer "not set" has to be cached as well as
 * "set": this runs on every pass of the NV2A poll loop, and on the default
 * path -- neither switch set -- a cache that only remembered "set" called
 * getenv on every pass, which walks the whole environment. */
static int pb_walk_enabled(void)
{
    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    if (s_scan_enabled < 0)
        s_scan_enabled = getenv("RECOMP_PB_SCAN") != NULL;
    return s_exec_enabled || s_scan_enabled;
}

/* Slot + 1 of each (subchannel, method) in s_seen. note() runs for every
 * pushbuffer word, so a linear search here was the executor's largest cost
 * (about 40% of its thread in X-Men Legends' levels). */
static uint16_t s_seen_slot[8][0x2000 / 4];

/* Diagnostic timing stays outside the executor: a packet still reaches the
 * same decoder and methods, in the same order. Time a header's whole payload
 * rather than every vertex parameter, so tracing does not add two clocks per
 * attribute. Only a slow submission (or gap between submissions) is printed. */
typedef struct {
    int64_t started, cpu_started, packet_ticks;
    int64_t max_ticks, max_started, flip_ticks;
    uint32_t packets, max_va, max_method, max_subch, max_count, max_param;
} PB_HITCH;
static int64_t s_hitch_frequency, s_hitch_previous_end;

/* QueryPerformanceCounter and GetThreadTimes come from win32_compat on POSIX
 * (CLOCK_MONOTONIC and CLOCK_THREAD_CPUTIME_ID there). */
static int pb_hitch_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *setting = getenv("RECOMP_HITCH_TRACE");
        enabled = setting && *setting && strcmp(setting, "0") != 0;
        if (enabled) {
            LARGE_INTEGER frequency;
            QueryPerformanceFrequency(&frequency);
            s_hitch_frequency = frequency.QuadPart;
        }
    }
    return enabled;
}

static int64_t pb_hitch_clock(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static int64_t pb_hitch_cpu(void)
{
    FILETIME created, exited, kernel, user;
    if (GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return (((int64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime)
             + (((int64_t)user.dwHighDateTime << 32) | user.dwLowDateTime);
    return 0;
}

static void pb_hitch_report(PB_HITCH *hitch, uint32_t start, uint32_t end,
                            uint32_t words)
{
    int64_t finished = pb_hitch_clock();
    int64_t cpu = pb_hitch_cpu() - hitch->cpu_started;
    double ms = 1000.0 / (double)s_hitch_frequency;
    double wall = (finished - hitch->started) * ms;
    double gap = s_hitch_previous_end
        ? (hitch->started - s_hitch_previous_end) * ms : 0;
    unsigned long thread = 0;
#ifdef _WIN32
    thread = (unsigned long)GetCurrentThreadId();
#endif
    s_hitch_previous_end = finished;
    if (wall < 33.0 && gap < 33.0) return;
    fprintf(stderr,
        "[HITCH] phase=pb_submission qpc_ms=%.3f start_qpc_ms=%.3f "
        "duration_ms=%.3f thread=%lu span=%08X-%08X cpu_ms=%.3f "
        "gap_ms=%.3f words=%u packets=%u packet_ms=%.3f flip_ms=%.3f "
        "max_ms=%.3f max_qpc_ms=%.3f va=%08X method=%04X subch=%u "
        "count=%u param=%08X\n",
        finished * ms, hitch->started * ms, wall, thread, start, end,
        cpu / 10000.0, gap, words,
        hitch->packets, hitch->packet_ticks * ms, hitch->flip_ticks * ms,
        hitch->max_ticks * ms, hitch->max_started * ms, hitch->max_va,
        hitch->max_method, hitch->max_subch, hitch->max_count, hitch->max_param);
}

static void note(uint32_t subch, uint32_t method)
{
    uint16_t *slot = &s_seen_slot[subch & 7][(method & 0x1FFC) / 4];

    if (*slot) {
        s_seen[*slot - 1].count++;
        return;
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
        *slot = (uint16_t)s_seen_count;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1818 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "ARRAY_ELEMENT32" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1818, "INLINE_ARRAY" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

/* Every word the walk reads must lie in the 64 MB contiguous window. A real
 * stream lives in RAM, so a JUMP or CALL target above it is a parameter read
 * as a command after a desync. Wrapping it would execute whatever sits at the
 * wrapped address and corrupt GPU state quietly; the walk stops there
 * instead, as PFIFO does at its DMA limit (xemu's pfifo.c raises a protection
 * error; it never wraps). apexJCL's walker (blinx2-v0.4.0) made this choice;
 * MM3 never leaves the window on either. */
static int pb_in_window(uint32_t va)
{
    return (va & 3u) == 0 && va - XBOX_CONTIG_BASE <= XBOX_CONTIG_SIZE - 4u;
}

/* The words from a stop to PUT are never executed, and they may hold the
 * semaphore release the title's next fence wait spins on. Run the last such
 * release in [from, to): releasing it marks the earlier ones done too, and
 * none beyond PUT.
 *
 * The XDK writes a fence as (BACK_END_WRITE_SEMAPHORE_RELEASE, count 1,
 * subchannel 0) and its value, but that pair can also be data (index buffer
 * words). A value taken from data could push the fence far ahead, so a
 * candidate counts only if it looks like the next fence: odd, as the XDK's
 * fence values are, after the last release, and at most 128 past it. Returns
 * how the fence was handled, for the log. A span that wraps or can't be read
 * is left to the fence mirror. */
static const char *pb_stop_salvage(const uint8_t *mem, uint32_t from, uint32_t to,
                                   uint32_t *value)
{
    uint32_t va, last, rel;
    int found = 0, seen = 0;

    if (!s_exec_enabled)
        return "not executing";
    if (from >= to || !pb_in_window(from) || !pb_in_window(to - 4))
        return "span wraps or is unreadable; fence left to the mirror";
    last = nv2a_pb_exec_semaphore_last_value();
    for (va = from; va + 4 < to; va += 4) {
        if (*(const uint32_t *)(mem + va) == 0x00041D70u) {
            uint32_t v = *(const uint32_t *)(mem + va + 4);
            seen++;
            if ((v & 1u) && (int32_t)(v - last) > 0 && v - last <= 128u) {
                last = v;
                found = 1;
            }
        }
    }
    if (!found)
        return seen ? "release(s) in the rest but none plausible"
                    : "no fence release in the rest";
    rel = nv2a_pb_exec_semaphore_releases();
    nv2a_pb_exec_method(0, 0x1D70u, last);
    *value = last;
    if (nv2a_pb_exec_semaphore_releases() == rel)
        return "fence release found but refused by the executor";
    s_tot_salvaged++;
    return "fence release salvaged";
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0) {
        nv2a_pb_exec_report();
        fprintf(stderr, "[PB] %u pushbuffer calls followed\n", s_tot_calls);
        fprintf(stderr, "[PB] %u walks stopped short of PUT (%u with a fence"
                        " release salvaged)\n", s_tot_stops, s_tot_salvaged);
        if (s_tot_refused)
            fprintf(stderr, "[PB] %u segments refused: GET or PUT outside the"
                            " contiguous window\n", s_tot_refused);
    }
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u calls,"
                    " %u unrecognised -- %d distinct (subchannel, method)"
                    " pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_calls,
            s_tot_unknown, s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

static int s_scan = -1;

/* DMA_PUT may expose only part of a method packet. PFIFO resumes the
 * remaining parameters rather than interpreting them as new headers.
 * The same PUT boundary applies while a subroutine is active: CALL saves
 * one return address, not a separate FIFO with an invented four-MiB end. */
typedef struct {
    uint32_t next_va, packet_va, subch, method, remaining;
    int active, noninc;
} PB_PACKET_CONTINUATION;
static PB_PACKET_CONTINUATION s_packet;
typedef struct {
    uint32_t next_start, pc, return_va, last_control, last_target;
    int active, in_call;
} PB_FIFO_CONTINUATION;
static PB_FIFO_CONTINUATION s_fifo;
static unsigned s_parse_split_reports, s_parse_limit_reports;

/* An isolated parser capture records the bytes actually fetched, before GPU
 * callbacks can change RAM. Keep the complete bounded pass, not just its last
 * headers: a bogus control address can lead to a million empty words. Nothing
 * allocates, reads extra RAM, timestamps or performs I/O when disabled. */
typedef struct {
    uint32_t va, word, method, remaining, flags, return_va;
} PB_CAPTURE_READ;
static struct {
    const char *prefix;
    PB_CAPTURE_READ *first, recent[256];
    uint8_t *ring;
    uint32_t ring_va, ring_bytes, ring_capacity;
    XBOX_NV2A_CAPTURE_STATE initial_state;
    uint32_t first_count, recent_count, recent_next, start, end, pc;
    PB_PACKET_CONTINUATION packet;
    PB_FIFO_CONTINUATION fifo;
    int configured, recording, done;
} s_capture;

static void pb_capture_begin(uint32_t start, uint32_t end, uint32_t pc)
{
    if (!s_capture.configured) {
        const char *setting = getenv("RECOMP_PB_CAPTURE");
        s_capture.configured = 1;
        if (setting && *setting) {
            s_capture.first = malloc(0x100000u * sizeof *s_capture.first);
            if (s_capture.first) s_capture.prefix = setting;
        }
    }
    s_capture.recording = s_capture.prefix && !s_capture.done;
    if (!s_capture.recording) return;
    s_capture.first_count = 0;
    s_capture.start = start; s_capture.end = end; s_capture.pc = pc;
    s_capture.packet = s_packet; s_capture.fifo = s_fifo;
    /* This is evidence only: execution continues reading live guest RAM.
     * Preserve a bounded copy of the producer's configured ring before any
     * executor callbacks, to distinguish stale submissions from later reuse. */
    xbox_Nv2aCaptureSnapshot(&s_capture.initial_state);
    s_capture.ring_bytes = 0;
    for (uint32_t i = 0; i < s_capture.initial_state.mirror_count &&
                         i < XBOX_NV2A_CAPTURE_MIRRORS; i++) {
        const XBOX_NV2A_CAPTURE_MIRROR *m = &s_capture.initial_state.mirrors[i];
        uint32_t low, high, bytes;
        uint8_t *copy;
        if (m->device_bytes < 0x2C) continue;
        memcpy(&low, m->device + 0x24, sizeof low);
        memcpy(&high, m->device + 0x28, sizeof high);
        if (low < XBOX_CONTIG_BASE || high < low ||
            high > XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE - 4u) continue;
        bytes = high - low + 4u; /* Include the boundary's possible JUMP word. */
        if (bytes > 4u * 1024u * 1024u) continue;
        if (s_capture.ring_capacity < bytes) {
            copy = realloc(s_capture.ring, bytes);
            if (!copy) continue;
            s_capture.ring = copy; s_capture.ring_capacity = bytes;
        }
        memcpy(s_capture.ring, (const uint8_t *)xbox_GetMemoryOffset() + low, bytes);
        s_capture.ring_va = low; s_capture.ring_bytes = bytes;
        break;
    }
}

static void pb_capture_read(uint32_t va, uint32_t word, uint32_t method,
                            uint32_t remaining, int payload, int in_call,
                            uint32_t return_va)
{
    PB_CAPTURE_READ entry;
    entry.va = va; entry.word = word; entry.method = method;
    entry.remaining = remaining;
    entry.flags = (payload ? 1u : 0u) | (in_call ? 2u : 0u);
    entry.return_va = return_va;
    if (s_capture.first_count < 0x100000u)
        s_capture.first[s_capture.first_count++] = entry;
    s_capture.recent[s_capture.recent_next] = entry;
    s_capture.recent_next = (s_capture.recent_next + 1) & 255u;
    if (s_capture.recent_count < 256) s_capture.recent_count++;
}

static int pb_capture_write(const char *suffix, const void *data, size_t size)
{
    char path[4096];
    FILE *file;
    int result, length = snprintf(path, sizeof path, "%s%s", s_capture.prefix, suffix);
    if (length < 0 || length >= (int)sizeof path) return 0;
    file = fopen(path, "wb");
    if (!file) return 0;
    result = fwrite(data, 1, size, file) == size;
    if (fclose(file) != 0) result = 0;
    return result;
}

static uint32_t pb_capture_device_word(const XBOX_NV2A_CAPTURE_MIRROR *m,
                                       uint32_t offset)
{
    uint32_t value = 0;
    if (m->device_bytes >= offset + sizeof value)
        memcpy(&value, m->device + offset, sizeof value);
    return value;
}

static int pb_capture_state_json(char *buffer, size_t capacity,
                                 const XBOX_NV2A_CAPTURE_STATE *state)
{
    int n, used = 0;
#define PB_CAPTURE_JSON(...) do { \
    n = snprintf(buffer + used, capacity - (size_t)used, __VA_ARGS__); \
    if (n < 0 || (size_t)n >= capacity - (size_t)used) return 0; \
    used += n; \
} while (0)
    PB_CAPTURE_JSON("{\"version\":%u,\"bytes\":%u,\"mmio_valid\":%u,"
        "\"user_put\":%u,\"user_get\":%u,\"pfifo_put\":%u,\"pfifo_get\":%u,"
        "\"pfifo_subroutine\":%u,\"get_held\":%u,\"get_held_value\":%u,"
        "\"scan_incomplete\":%u,\"notify_count\":%u,\"notify_param\":%u,"
        "\"notify_device_ptr\":%u,\"notify_event_offset\":%u,"
        "\"counter_mirrors\":%u,\"frame_counters\":%u,\"mirrors\":[",
        state->version, state->bytes, state->mmio_valid, state->user_put,
        state->user_get, state->pfifo_put, state->pfifo_get, state->pfifo_subroutine,
        state->get_held, state->get_held_value, state->scan_incomplete,
        state->notify_count, state->notify_param, state->notify_device_ptr,
        state->notify_event_offset, state->counter_mirrors, state->frame_counters);
    for (uint32_t i = 0; i < state->mirror_count && i < XBOX_NV2A_CAPTURE_MIRRORS; i++) {
        const XBOX_NV2A_CAPTURE_MIRROR *m = &state->mirrors[i];
        PB_CAPTURE_JSON("%s{\"device_ptr_va\":%u,\"device_va\":%u,"
            "\"device_valid\":%u,\"device_bytes\":%u,\"put_offset\":%u,"
            "\"get_ptr_offset\":%u,\"put_value\":%u,\"fence_va\":%u,"
            "\"fence_value\":%u,\"fence_valid\":%u,\"channel_va\":%u,"
            "\"channel_valid\":%u,\"channel_put\":%u,\"channel_get\":%u,"
            "\"pfifo_va\":%u,\"pfifo_valid\":%u,\"pfifo_subroutine\":%u,"
            "\"command_mode\":%u,\"command_dma\":%u,\"command_offset\":%u,"
            "\"command_pending\":%u,\"command_value\":%u,"
            "\"command_fence_va\":%u,\"command_queue_count\":%u,"
            "\"cursor\":%u,\"limit\":%u,\"flags\":%u,\"ring_low\":%u,"
            "\"ring_high\":%u,\"next_fence\":%u,\"fence_pointer\":%u,"
            "\"fence_index\":%u,\"fence_mask\":%u,\"ring_used\":%u,"
            "\"fence_table\":%u}", i ? "," : "",
            m->device_ptr_va, m->device_va, m->device_valid, m->device_bytes,
            m->put_offset, m->get_ptr_offset, m->put_value, m->fence_va,
            m->fence_value, m->fence_valid, m->channel_va, m->channel_valid,
            m->channel_put, m->channel_get, m->pfifo_va, m->pfifo_valid,
            m->pfifo_subroutine, m->command_mode, m->command_dma, m->command_offset,
            m->command_pending, m->command_value, m->command_fence_va,
            m->command_queue_count, pb_capture_device_word(m, 0),
            pb_capture_device_word(m, 4), pb_capture_device_word(m, 8),
            pb_capture_device_word(m, 0x24), pb_capture_device_word(m, 0x28),
            pb_capture_device_word(m, 0x2C), pb_capture_device_word(m, 0x30),
            pb_capture_device_word(m, 0x34), pb_capture_device_word(m, 0x38),
            pb_capture_device_word(m, 0x40), pb_capture_device_word(m, 0x48));
    }
    PB_CAPTURE_JSON("]}");
#undef PB_CAPTURE_JSON
    return used;
}

static void pb_capture_limit(uint32_t words)
{
    char metadata[3072];
    PB_CAPTURE_READ recent[256];
    XBOX_NV2A_CAPTURE_STATE final_state;
    char producer[16384];
    const uint8_t *mem;
    int ram_ok, first_ok, recent_ok, json_ok, length;
    int ring_ok, initial_state_ok, final_state_ok, producer_ok;
#ifdef _WIN32
    DWORD previous_error;
#endif
    if (!s_capture.recording || s_capture.done) return;
#ifdef _WIN32
    previous_error = GetLastError();
#endif
    s_capture.done = 1; s_capture.recording = 0;
    xbox_Nv2aCaptureSnapshot(&final_state);
    mem = (const uint8_t *)xbox_GetMemoryOffset();
    for (uint32_t i = 0; i < s_capture.recent_count; i++)
        recent[i] = s_capture.recent[(s_capture.recent_next + 256u -
                                     s_capture.recent_count + i) & 255u];
    ram_ok = pb_capture_write(".ram.bin", mem + XBOX_CONTIG_BASE, XBOX_CONTIG_SIZE);
    first_ok = pb_capture_write(".reads.bin", s_capture.first,
                               s_capture.first_count * sizeof *s_capture.first);
    recent_ok = pb_capture_write(".recent.bin", recent,
                                s_capture.recent_count * sizeof *recent);
    ring_ok = s_capture.ring_bytes && pb_capture_write(".initial-ring.bin",
                                  s_capture.ring, s_capture.ring_bytes);
    initial_state_ok = pb_capture_write(".state-initial.bin",
                    &s_capture.initial_state, sizeof s_capture.initial_state);
    final_state_ok = pb_capture_write(".state-final.bin", &final_state, sizeof final_state);
    memcpy(producer, "{\"initial\":", 11);
    length = pb_capture_state_json(producer + 11, sizeof producer - 11,
                                   &s_capture.initial_state);
    producer_ok = 0;
    if (length && 11u + (size_t)length + 11u < sizeof producer) {
        size_t used = 11u + (size_t)length;
        memcpy(producer + used, ",\"final\":", 9); used += 9;
        length = pb_capture_state_json(producer + used, sizeof producer - used - 2,
                                       &final_state);
        if (length) {
            used += (size_t)length;
            memcpy(producer + used, "}\n", 2); used += 2;
            producer_ok = pb_capture_write(".producer.json", producer, used);
        }
    }
    length = snprintf(metadata, sizeof metadata,
        "{\n\"version\":1,\"contiguous_base\":%u,\"ram_bytes\":%u,"
        "\"record_bytes\":%u,\"record_fields\":[\"va\",\"word\",\"method\","
        "\"remaining\",\"flags\",\"return_va\"],\"payload_flag\":1,\"in_call_flag\":2,\n"
        "\"start\":%u,\"end\":%u,\"initial_pc\":%u,\"words\":%u,"
        "\"first_reads\":%u,\"recent_reads\":%u,\n"
        "\"initial_fifo\":{\"active\":%d,\"in_call\":%d,\"return_va\":%u,"
        "\"last_control\":%u,\"last_target\":%u},\n"
        "\"initial_packet\":{\"active\":%d,\"noninc\":%d,\"packet_va\":%u,"
        "\"method\":%u,\"subch\":%u,\"remaining\":%u},\n"
        "\"final_fifo\":{\"pc\":%u,\"in_call\":%d,\"return_va\":%u,"
        "\"last_control\":%u,\"last_target\":%u},\n"
        "\"final_packet\":{\"active\":%d,\"noninc\":%d,\"packet_va\":%u,"
        "\"method\":%u,\"subch\":%u,\"remaining\":%u},\n"
        "\"ram_ok\":%d,\"first_ok\":%d,\"recent_ok\":%d,\n"
        "\"initial_ring_va\":%u,\"initial_ring_bytes\":%u,\"initial_ring_ok\":%d,"
        "\"state_bytes\":%u,\"initial_state_ok\":%d,\"final_state_ok\":%d,"
        "\"producer_ok\":%d\n}\n",
        (unsigned)XBOX_CONTIG_BASE, (unsigned)XBOX_CONTIG_SIZE,
        (unsigned)sizeof(PB_CAPTURE_READ), s_capture.start, s_capture.end, s_capture.pc,
        words, s_capture.first_count, s_capture.recent_count,
        s_capture.fifo.active, s_capture.fifo.in_call, s_capture.fifo.return_va,
        s_capture.fifo.last_control, s_capture.fifo.last_target,
        s_capture.packet.active, s_capture.packet.noninc, s_capture.packet.packet_va,
        s_capture.packet.method, s_capture.packet.subch, s_capture.packet.remaining,
        s_fifo.pc, s_fifo.in_call, s_fifo.return_va, s_fifo.last_control, s_fifo.last_target,
        s_packet.active, s_packet.noninc, s_packet.packet_va,
        s_packet.method, s_packet.subch, s_packet.remaining, ram_ok, first_ok, recent_ok,
        s_capture.ring_va, s_capture.ring_bytes, ring_ok, (unsigned)sizeof final_state,
        initial_state_ok, final_state_ok, producer_ok);
    json_ok = length >= 0 && length < (int)sizeof metadata &&
              pb_capture_write(".json", metadata, (size_t)length);
    fprintf(stderr, "[PB_CAPTURE] prefix=%s words=%u reads=%u ram_ok=%d "
                    "first_ok=%d recent_ok=%d json_ok=%d\n",
            s_capture.prefix, words, s_capture.first_count,
            ram_ok, first_ok, recent_ok, json_ok);
    free(s_capture.first); s_capture.first = NULL;
    free(s_capture.ring); s_capture.ring = NULL; s_capture.ring_capacity = 0;
#ifdef _WIN32
    SetLastError(previous_error);
#endif
}

/* Walk one PFIFO, including its single-level CALL/RETURN state. The work cap
 * is a host scheduling bound, not an acknowledgment of unexecuted commands.
 * A caller must retry the saved PC until it reaches the actual DMA_PUT. */
static uint32_t pb_walk(uint32_t va, uint32_t end_va,
                        uint32_t *jumps, uint32_t *unknown, PB_HITCH *hitch)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t words = 0;
    uint32_t last_control = s_fifo.last_control, last_target = s_fifo.last_target;
    uint32_t return_va = s_fifo.return_va;
    int in_call = s_fifo.in_call;
    const char *why = NULL;
    uint32_t at = va, word = 0;

    while (va != end_va && words < 0x100000u) {
        uint32_t count, subch, method, packet_va;
        int noninc;
        if (s_packet.active) {
            count = s_packet.remaining; subch = s_packet.subch;
            method = s_packet.method; noninc = s_packet.noninc;
            packet_va = s_packet.packet_va;
            s_packet.active = 0;
        } else {
            uint32_t w;
            if (!pb_in_window(va)) {
                why = "outside the contiguous window"; at = va;
                goto stop;
            }
            w = *(const uint32_t *)(mem + va);
            if (s_capture.recording)
                pb_capture_read(va, w, 0, 0, 0, in_call, return_va);
            packet_va = va;
            va += 4;
            words++;

            if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
                uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                                 : (w & 0x1FFFFFFCu);
                if (target >= XBOX_CONTIG_SIZE) {
                    why = "jump outside the window"; at = packet_va; word = w;
                    goto stop;
                }
                last_control = w; last_target = XBOX_CONTIG_BASE | target;
                (*jumps)++;
                va = last_target;
                continue;
            }
            if (w == 0x00020000u) {                 /* exact RETURN */
                if (in_call) {
                    va = return_va;
                    in_call = 0;
                }
                continue;
            }
            if ((w & 3u) == 2u) {                    /* CALL */
                if ((w & 0xFFFFFFFCu) >= XBOX_CONTIG_SIZE) {
                    why = "call outside the window"; at = packet_va; word = w;
                    goto stop;
                }
                last_control = w; last_target = XBOX_CONTIG_BASE | (w & 0xFFFFFFFCu);
                if (!in_call) {
                    s_tot_calls++;
                    return_va = va;
                    in_call = 1;
                    va = last_target;
                }
                continue;
            }
            if ((w & 0xE0030003u) != 0u &&
                (w & 0xE0030003u) != 0x40000000u) {
                (*unknown)++;
                continue;
            }
            count = (w >> 18) & 0x7FFu;
            subch = (w >> 13) & 7u;
            method = w & 0x1FFCu;
            noninc = (w & 0xE0000000u) == 0x40000000u;
        }
        {
            uint32_t first_param = 0, i;
            int64_t packet_start = hitch && count ? pb_hitch_clock() : 0;
            int contains_flip = 0;

            for (i = 0; i < count && va != end_va && words < 0x100000u; i++) {
                uint32_t m = noninc ? method : (method + i * 4) & 0x1FFCu;
                if (hitch && m == 0x0130) contains_flip = 1;
                if (s_scan)  /* the inventory is a linear search: survey runs only */
                    note(subch, m);
                /* Same walk, two consumers: the survey counts, the executor
                 * acts. Keeping them on one decode means they can never
                 * disagree about what the stream said. */
                if (!pb_in_window(va)) {
                    why = "outside the contiguous window"; at = va;
                    goto stop;
                }
                if (s_exec_enabled || s_capture.recording) {
                    uint32_t param = *(const uint32_t *)(mem + va);
                    if (hitch && !i) first_param = param;
                    if (s_capture.recording)
                        pb_capture_read(va, param, m, count - i, 1, in_call, return_va);
                    if (s_exec_enabled) nv2a_pb_exec_method(subch, m, param);
                }
                va += 4;
                words++;
            }
            if (i < count) {
                s_packet.active = 1; s_packet.next_va = va;
                s_packet.packet_va = packet_va; s_packet.subch = subch;
                s_packet.method = noninc ? method : (method + i * 4) & 0x1FFCu;
                s_packet.remaining = count - i; s_packet.noninc = noninc;
                if (hitch && s_parse_split_reports++ < 64) {
                    int64_t now = pb_hitch_clock();
                    fprintf(stderr, "[PB_PARSE] phase=packet_split qpc_ms=%.3f "
                        "packet_va=%08X next_va=%08X method=%04X subch=%u remaining=%u\n",
                        now * 1000.0 / s_hitch_frequency, packet_va, va,
                        s_packet.method, subch, s_packet.remaining);
                }
            }
            if (hitch) {
                int64_t ticks = count ? pb_hitch_clock() - packet_start : 0;
                hitch->packets++;
                hitch->packet_ticks += ticks;
                /* FLIP_STALL includes normal vblank pacing. Keep it separate
                 * so a paced flip cannot hide a slow draw/visibility packet. */
                if (contains_flip) {
                    hitch->flip_ticks += ticks;
                } else if (ticks > hitch->max_ticks) {
                    hitch->max_ticks = ticks;
                    hitch->max_started = packet_start;
                    hitch->max_va = packet_va;
                    hitch->max_method = method;
                    hitch->max_subch = subch;
                    hitch->max_count = count;
                    hitch->max_param = first_param;
                }
            }
        }
    }
    if (0) {
stop:   /* Out of step: drop the method in progress and the call, run the
         * fence release the rest may hold, and resume at PUT. Logged once
         * per stop, the first eight times. */
        {
            static unsigned said;
            uint32_t v = 0;
            const char *fence = pb_stop_salvage(mem, at, end_va, &v);
            s_packet.active = 0; in_call = 0;
            s_tot_stops++;
            if (said++ < 8)
                fprintf(stderr, "[PB] walk stopped at 0x%08X (word 0x%08X) after %u"
                        " words, PUT 0x%08X (%s); %s 0x%08X\n",
                        at, word, words, end_va, why, fence, v);
            va = end_va;
        }
    }
    s_fifo.pc = va; s_fifo.in_call = in_call; s_fifo.return_va = return_va;
    s_fifo.last_control = last_control; s_fifo.last_target = last_target;
    if (va != end_va && words >= 0x100000u) pb_capture_limit(words);
    if (hitch && va != end_va && words >= 0x100000u &&
        s_parse_limit_reports++ < 16) {
        int64_t now = pb_hitch_clock();
        fprintf(stderr, "[PB_PARSE] phase=parse_limit qpc_ms=%.3f pc=%08X "
            "end_va=%08X in_call=%d words=%u last_control=%08X target=%08X\n",
            now * 1000.0 / s_hitch_frequency, va, end_va, in_call, words,
            last_control, last_target);
    }
    return words;
}

int nv2a_pb_scan(uint32_t start_va, uint32_t end_va)
{
    uint32_t words, jumps = 0, unknown = 0;
    uint32_t pc;
    int completed;
    PB_HITCH trace, *hitch = NULL;

    /* GET and PUT must lie in the contiguous window. A value past it is not
     * an alias of a low address: the walk cannot read it (see pb_in_window),
     * and wrapping it would run whatever sits at the wrapped address. Refuse
     * the segment and log the first few. Report it consumed, not incomplete:
     * an incomplete scan is retried every tick with the same refusal and
     * holds back every fence publish, which is the silent stall. */
    if (!pb_in_window(start_va) || end_va - XBOX_CONTIG_BASE > XBOX_CONTIG_SIZE) {
        s_tot_refused++;
        if (s_tot_refused <= 8)
            fprintf(stderr, "[PB] refused segment GET 0x%08X PUT 0x%08X: outside"
                            " the 64 MB contiguous window (%u refused so far)\n",
                    start_va, end_va, s_tot_refused);
        return 1;
    }

    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    if (s_scan < 0)
        s_scan = getenv("RECOMP_PB_SCAN") != NULL;
    if (!(s_scan || s_exec_enabled) || end_va == start_va)
        return 1;

    /* A fresh/discontinuous scan does not inherit another FIFO's fragment. */
    if (s_fifo.active && s_fifo.next_start == start_va) {
        pc = s_fifo.pc;
    } else {
        pc = start_va;
        s_packet.active = 0;
        memset(&s_fifo, 0, sizeof s_fifo);
    }
    pb_capture_begin(start_va, end_va, pc);

    if (pb_hitch_enabled()) {
        hitch = &trace;
        memset(&trace, 0, sizeof trace);
        trace.cpu_started = pb_hitch_cpu();
        trace.started = pb_hitch_clock();
    }

    {
        extern void nv2a_d3d_kick(void);
        nv2a_d3d_kick();
    }
    RECOMP_PROFILE_BEGIN("GPU submission");
    words = pb_walk(pc, end_va, &jumps, &unknown, hitch);
    RECOMP_PROFILE_END();
    if (hitch) pb_hitch_report(hitch, start_va, end_va, words);

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
    completed = s_fifo.pc == end_va;
    s_fifo.next_start = completed ? end_va : start_va;
    s_fifo.active = !completed || s_fifo.in_call || s_packet.active;
    return completed;
}
