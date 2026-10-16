#ifndef NV2A_HOST_COMMANDS_H
#define NV2A_HOST_COMMANDS_H

/* A host fog pass is ordered in the title's pushbuffer using eleven
 * non-increasing NV097_NO_OPERATION parameters on subchannel zero:
 * tag, float W-to-world scale, far clip, start, end, packed ARGB, flags,
 * world-up ray plane XYZ and inverse sky-fade width.
 * The ordinary Xbox command decoder treats this packet as no-ops. */
#define NV2A_HOST_FOG_HEADER 0x402C0100u
#define NV2A_HOST_FOG_TAG 0x58424647u
#define NV2A_HOST_FOG_WORDS 12u
#define NV2A_HOST_FOG_SKY_MASK 1u /* Destination alpha: 1 at clear depth, 0 at geometry. */
#define NV2A_HOST_FOG_SKY_FILL 2u /* Fill clear-depth background with the fog colour. */
#define NV2A_HOST_FOG_SKY_FADE 4u /* Fade later sky composition above the world horizon. */

#endif
