/* Idle-voice traps take turns (mcpx_apu_vp_frame in the real apu_vp.c).
 *
 * The front end traps on one idle voice per frame and the title's ISR may
 * decline to free it (DirectSound keeps a voice whose buffer is busy, or a
 * stream's non-last voice). Starting from the list head every frame, such a
 * voice took every trap and no idle voice behind it was ever reported, so
 * none was freed and sounds died one by one. Here the head voice is always
 * declined and the ISR frees every other voice it is told about: all of them
 * must be reported, and freed, within a few frames. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/apu/apu_vp.c"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "VP idle trap check line %d: %s\n", __LINE__, #condition); \
    exit(1); } } while (0)

struct McpxApuDebug g_dbg, g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4];
static uint8_t ram[0x10000];
uint8_t *g_apu_ram_ptr = ram;
uint8_t *mcpx_apu_phys(uint64_t address)
{
    CHECK(address < sizeof(ram)); /* voice structs only */
    return ram + address;
}
/* apu_vp.c's diagnostics, off here. */
const char *mcpx_apu_phys_class(uint64_t address) { (void)address; return "none"; }
int apu_env_on(const char *name) { (void)name; return 0; }
ApuIrqStats apu_irq_stats;

#define VOICES 4
static const uint16_t handles[VOICES] = { 0xF2, 0xF3, 0xF4, 0xF5 };

/* The 2D list holds the voices still listed, in handle order. */
static void link_list(MCPXAPUState *d, const int listed[VOICES])
{
    uint16_t head = 0xFFFF;
    for (int i = VOICES - 1; i >= 0; i--) {
        if (!listed[i]) continue;
        voice_set_mask(d, handles[i], NV_PAVS_VOICE_TAR_PITCH_LINK,
                       NV_PAVS_VOICE_TAR_PITCH_LINK_NEXT_VOICE_HANDLE, head);
        head = handles[i];
    }
    d->regs[NV_PAPU_TVL2D] = head;
}

int main(void)
{
    static MCPXAPUState d;
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    int listed[VOICES] = { 1, 1, 1, 1 }, reported[VOICES] = { 0 };

    mcpx_apu_vp_init(&d);
    d.regs[NV_PAPU_VPVADDR] = 0x1000;
    d.regs[NV_PAPU_FETFORCE1] = NV_PAPU_FETFORCE1_SE2FE_IDLE_VOICE;
    d.regs[NV_PAPU_TVL3D] = 0xFFFF;
    d.regs[NV_PAPU_TVLMP] = 0xFFFF;
    link_list(&d, listed);   /* all idle: PAR_STATE is 0 in zeroed RAM */

    for (int frame = 0; frame < 2 * VOICES; frame++) {
        mcpx_apu_vp_frame(&d, mixbins);
        if ((d.regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE)
                != NV_PAPU_FECTL_FEMETHMODE_TRAPPED)
            break;   /* nothing idle left to report */
        uint32_t voice = d.regs[NV_PAPU_FEDECPARAM];
        int i = 0;
        while (i < VOICES && handles[i] != voice) i++;
        CHECK(i < VOICES && listed[i]);
        reported[i]++;
        if (i != 0) {   /* the ISR frees it; the head voice it always declines */
            listed[i] = 0;
            link_list(&d, listed);
        }
        d.regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE;   /* the ISR resumes */
    }
    for (int i = 1; i < VOICES; i++) {
        if (!reported[i])
            fprintf(stderr, "voice 0x%X behind the declined head was never reported\n",
                    handles[i]);
        CHECK(reported[i] == 1 && !listed[i]);
    }
    CHECK(reported[0] >= 1);   /* the declined voice still gets its turns */
    printf("VP idle traps: a declined head voice no longer hides the %d behind it\n",
           VOICES - 1);
    mcpx_apu_vp_finalize(&d);
    return 0;
}
