/* Diagnostics for indirect calls the dispatcher cannot resolve. */
#ifndef XBOX_ICALL_DIAGNOSTICS_H
#define XBOX_ICALL_DIAGNOSTICS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot passed to a report hook for each non-code target that was printed. */
typedef struct {
    unsigned sample;             /* per-process sequence number of this report */
    uint32_t target;             /* the Xbox VA the call tried to reach */
    uint32_t caller;             /* return address read from the guest stack, 0 if unknown */
    uint32_t esp;               /* guest ESP at the call */
    int stack_read;              /* stack_words is valid */
    uint32_t stack_words[16];    /* guest stack words from ESP */
} XboxIcallReport;

/* Print every report, bypassing the 50-line cap. Explicit tracing uses this. */
void xbox_icall_set_trace_all(int enabled);

/* Called after each non-code report that is printed (not capped). A title uses
 * this for caller-specific dumps; it must not change guest state. */
void xbox_icall_set_report_hook(void (*hook)(const XboxIcallReport *report));

/* Bounded guest read for diagnostics: never faults, returns 0 when the range is
 * not readable. Size is limited to 128 bytes. */
int xbox_icall_read(uint32_t va, void *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
