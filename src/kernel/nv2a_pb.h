#ifndef NV2A_PB_H
#define NV2A_PB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Optional application recovery for malformed submissions. Configure before
 * guest execution or while quiesced; NULL disables it. The handler runs
 * synchronously only in execution mode, after an invalid command/address,
 * before packet/call state is discarded and the scanner resumes at PUT.
 * stopped_va may be outside RAM or after put_va (wrapped submissions).
 * It is not a validated readable span. reason is borrowed for this call.
 * Do not reenter the scanner. Work-cap yields and partial packets do not call
 * this handler. No default handler guesses or executes skipped commands. */
typedef void (*NV2A_PB_STOP_HANDLER)(uint32_t stopped_va, uint32_t put_va,
                                      const char *reason);
void nv2a_pb_set_stop_handler(NV2A_PB_STOP_HANDLER handler);

/* Executor primitives for explicit caller policy; these do not validate a
 * command stream. Use only on its execution thread, without reentrancy. */
void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
uint32_t nv2a_pb_exec_semaphore_last_value(void);
uint32_t nv2a_pb_exec_semaphore_releases(void);

#ifdef __cplusplus
}
#endif
#endif
