#ifndef XBOX_HOST_H
#define XBOX_HOST_H

/* Call after xbox_kernel_bridge_init. Brings up the emulated APU (with
 * RECOMP_AC97_READY, which unmaps its registers to trap them) and the OHCI
 * USB controllers, and installs the vectored handler that routes guest MMIO
 * faults to them and to the NV2A. Returns 0 on failure. */
int  xbox_HostHardwareInit(void);
void xbox_HostHardwareShutdown(void);

/* Opens the game window, creates the D3D8 device the NV2A executor draws
 * through, and traps PVIDEO so its register writes take effect. Returns 0 on
 * failure. */
int  xbox_HostGraphicsInit(const char *window_title);

#endif
