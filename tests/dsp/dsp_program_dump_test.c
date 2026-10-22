/* The real automatic program export, driven by synthetic interpreted code. */
#include "dsp.h"
#include "dsp_port.h"
#include <errno.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"program dump line %d: %s\n",__LINE__,#c); exit(1); } } while (0)
static void scratch(void *o,uint8_t *p,uint32_t a,size_t n,bool dir)
{ (void)o;(void)p;(void)a;(void)n;(void)dir; CHECK(false); }
static void fifo(void *o,uint8_t *p,unsigned i,size_t n,bool dir)
{ (void)o;(void)p;(void)i;(void)n;(void)dir; CHECK(false); }
int main(int argc,char **argv)
{
    CHECK(argc==2);
    bool gp=!strcmp(argv[1],"gp");
    const char *directory=getenv("RECOMP_DSP_PROGRAM_DIR");
    char path[1024];
    CHECK(directory && *directory);
    int n=snprintf(path,sizeof path,"%s/%s.p.bin",directory,gp?"gp":"ep");
    CHECK(n>0 && n<sizeof path);
    /* Refuse stale output if the previous test-owned file cannot be removed. */
    CHECK(!remove(path) || errno==ENOENT);
    DSPState *d=dsp_init(NULL,scratch,fifo,gp); CHECK(d);
    dsp_write_memory(d,'P',0,0); /* nop */
    dsp_write_memory(d,'P',1,0x0c0000); /* jmp 0 */
    dsp_write_memory(d,'P',2,0x654321); /* untouched data, not program output */
    for(unsigned frame=0;frame<1600;frame++) {
        dsp_start_frame(d); dsp_run(d,2004);
        CHECK(d->core.pc<=1 && dsp_has_work(d));
    }
    uint32_t program[DSP_PRAM_SIZE];
    FILE *file=fopen(path,"rb"); CHECK(file);
    CHECK(fread(program,4,DSP_PRAM_SIZE,file)==DSP_PRAM_SIZE);
    CHECK(fgetc(file)==EOF && !fclose(file));
    CHECK(program[0]==0 && program[1]==0x0c0000);
    for(unsigned i=2;i<DSP_PRAM_SIZE;i++) CHECK(program[i]==0);
    CHECK(dsp_read_memory(d,'P',2)==0x654321);
    free(d);
    puts("PASS: automatic DSP export contains executed words and preserves guest data");
    return 0;
}
