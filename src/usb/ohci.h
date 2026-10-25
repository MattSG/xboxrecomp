/* OHCI host controller model for the two MCPX USB apertures.
 * Implements operational/root-hub registers, endpoint/transfer descriptor
 * walking and interrupt delivery, connected to the runtime's USB gamepads.
 * This is a partial device model; see ohci.c for supported transfer paths.
 */
#ifndef XBOX_OHCI_H
#define XBOX_OHCI_H

#include <stdint.h>

/* The two MCPX host controllers, as the XDK addresses them. */
#define XBOX_OHCI0_BASE   0xFED00000u
#define XBOX_OHCI1_BASE   0xFED08000u
#define XBOX_OHCI_SIZE    0x00001000u   /* 4 KB, the length XAPI registers */

/* Bring the models up. Safe to call more than once; does nothing unless
 * RECOMP_USB is set, so a title that was working without a controller keeps
 * behaving exactly as it did. */
void xbox_OhciInit(void);

/* 1 if the address is inside a controller this model owns. */
int  xbox_OhciOwnsAddress(uint32_t xbox_va);

/* Service a trapped access. Returns 1 if the faulting instruction was decoded
 * and stepped over. `ctx` is a PCONTEXT; void * keeps windows.h out of here. */
int  xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va);

/* Report counts at exit, so a run says whether the driver ever looked. */
void xbox_OhciReport(void);

/* Resolve one physical descriptor/buffer address to the guest VA used by the
 * OHCI model. Exposed so memory mapping regressions exercise the same path. */
uint32_t xbox_OhciResolvePhysical(uint32_t physical, uint32_t bytes);

#endif /* XBOX_OHCI_H */
