/* Standalone renderer harnesses do not run the host window or sampler. */
#ifndef NV2A_TEST_HOST_H
#define NV2A_TEST_HOST_H
int xbox_FramebufferPaused(void) { return 0; }
void recomp_sampler_watch_current_thread(void) {}
void recomp_sampler_report(double ms) { (void)ms; }
#endif
