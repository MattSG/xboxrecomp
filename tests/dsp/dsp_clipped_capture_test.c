#include "dsp.h"
#include "dsp_port.h"
#include "dsp_dma_regs.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"clipped capture line %d\n",__LINE__); exit(1); } } while (0)
static void scratch(void *o, uint8_t *p, uint32_t a, size_t n, bool dir) { (void)o;(void)p;(void)a;(void)n;(void)dir; CHECK(false); }
static void fifo(void *o, uint8_t *p, unsigned i, size_t n, bool dir) { (void)o;(void)p; CHECK(i == 0 && n == 4 && dir); }
int main(void) {
    DSPState *d = dsp_init(NULL, scratch, fifo, false); CHECK(d);
    uint32_t program[] = {0x44f400,0x20,0x08c414,0x44f400,1,0x08c416,0x44f400,1,0x08c404};
    uint32_t node[] = {1u<<14, NODE_CONTROL_DIRECTION | (1u<<10), 2, 0x200, 0, 0, 0};
    for (unsigned i=0;i<9;i++) dsp_write_memory(d,'P',i,program[i]);
    for (unsigned i=0;i<7;i++) dsp_write_memory(d,'X',0x20+i,node[i]);
    uint32_t values[] = {0x123400,0x7fff00,0x567800};
    for (unsigned frame=0;frame<3;frame++) {
        d->core.pc=0; d->dma.eol=false;
        dsp_write_memory(d,'X',0x200,values[frame]); dsp_write_memory(d,'X',0x201,0);
        dsp_start_frame(d); dsp_run(d,1000); CHECK(d->core.is_idle && d->dma.eol && !d->dma.error);
    }
    char path[1024]; snprintf(path,sizeof(path),"%s.ep.frame.bin",getenv("RECOMP_DSP_FRAME_DUMP"));
    FILE *f=fopen(path,"rb"); CHECK(f); uint32_t h[3]; DSPState initial;
    CHECK(fread(h,sizeof(h),1,f)==1 && h[1]==DSP_FRAME_CAPTURE_VERSION);
    CHECK(fread(&initial,sizeof(initial),1,f)==1 && initial.core.xram[0x200]==0x7fff00);
    CHECK(!fclose(f)); free(d); return 0;
}
