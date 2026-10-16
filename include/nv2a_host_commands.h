#ifndef NV2A_HOST_COMMANDS_H
#define NV2A_HOST_COMMANDS_H

/* A host fog pass is ordered in the title's pushbuffer using seven
 * non-increasing NV097_NO_OPERATION parameters on subchannel zero:
 * tag, float W-to-world scale, far clip, start, end, packed ARGB, flags.
 * The ordinary Xbox command decoder treats this packet as no-ops. */
#define NV2A_HOST_FOG_HEADER 0x401C0100u
#define NV2A_HOST_FOG_TAG 0x58424647u
#define NV2A_HOST_FOG_WORDS 8u
#define NV2A_HOST_FOG_SKY_MASK 1u /* Destination alpha: 1 at clear depth, 0 at geometry. */

#endif
