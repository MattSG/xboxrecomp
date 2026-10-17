/* Benchmark candidate table indexing on validated real learned PS keys. */
#include "../../src/video/nv2a_d3d11.c"
#include "../nv2a_test_host.h"
ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }
HWND xbox_FramebufferWindowHandle(void) { return NULL; }
int xbox_FramebufferScanSource(uint32_t *va,uint32_t *pitch) { return 0; }
int xbox_FramebufferOverlay(XboxOverlay *ovl) { return 0; }
void xbox_FramebufferNoteFlip(uint32_t n) {}
void xbox_FramebufferRenderHostOverlay(void *device, void *context, void *target) {}
int xbox_GetDisplayMode(uint32_t *pitch, uint32_t *bpp, uint32_t *w, uint32_t *h) { (void)pitch; (void)bpp; (void)w; (void)h; return 0; }
const uint32_t (*nv2a_vsh_program(uint32_t *start,uint32_t *version))[4] { return NULL; }
const float (*nv2a_vsh_constants(uint32_t *version))[4] { return NULL; }
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)

static PsKey number(unsigned i) { PsKey k={0};k.fin0=i;return k; }
static void fixture(void)
{
    s_spec=calloc(PS_SPEC_SLOTS,sizeof *s_spec);CHECK(s_spec);
    InitializeCriticalSection(&s_spec_cs);InitializeConditionVariable(&s_spec_cv);
    s_spec_head=s_spec_tail=s_spec_warm_head=s_spec_warm_tail=0;
}
static PsSpec *ready(const PsKey *key)
{
    PsSpec *e=spec_find(key);CHECK(e);
    e->key=*key;e->ps=(ID3D11PixelShader *)e;InterlockedExchange(&e->state,2);return e;
}
static DWORD WINAPI publish(void *arg)
{
    PsSpec *e=(PsSpec *)arg;Sleep(1);e->ps=(ID3D11PixelShader *)e;
    InterlockedExchange(&e->state,2);return 0;
}
static void test_lookup(void)
{
    PsKey k=number(1);PsSpec *first=ready(&k),*e;unsigned i;
    CHECK(spec_find(&k)==first && spec_request(&k,0)==first->ps);
    /* Every key byte participates in identity, regardless of hash collisions. */
    for(i=0;i<sizeof k;i++) {
        PsKey changed=k;((uint8_t *)&changed)[i]^=0x80;e=ready(&changed);
        CHECK(e!=first && spec_find(&changed)==e && spec_find(&k)==first);
    }
    /* Ready publication must become visible to repeated front-cache requests. */
    k=number(900);CHECK(!spec_request(&k,0));e=spec_find(&k);CHECK(e->state==1);
    InterlockedExchange(&e->state,4);CHECK(!spec_request(&k,0));
    e->ps=(ID3D11PixelShader *)e;InterlockedExchange(&e->state,2);
    CHECK(spec_request(&k,0)==e->ps);InterlockedExchange(&e->state,3);
    CHECK(!spec_request(&k,0));
    k=number(901);CHECK(!spec_request(&k,0));e=spec_find(&k);
    InterlockedExchange(&e->state,4);
    {
        HANDLE worker=CreateThread(NULL,0,publish,e,0,NULL);DWORD until=GetTickCount()+5000;
        CHECK(worker);
        while(!spec_request(&k,0) && (LONG)(GetTickCount()-until)<0) SwitchToThread();
        CHECK(WaitForSingleObject(worker,5000)==WAIT_OBJECT_0 && spec_request(&k,0)==e->ps);
        CloseHandle(worker);
    }
    /* Force an actual four-lane bucket collision, not just random coverage. */
    {
        uint64_t h=hash_mem((const uint8_t *)&k,sizeof k);unsigned bucket=(unsigned)(h^(h>>32))&(PS_SPEC_SLOTS-1);
        PsKey other;PsSpec *a=ready(&k),*b;
        for(i=902;i<100000;i++) {other=number(i);h=hash_mem((const uint8_t *)&other,sizeof other);
            if(((unsigned)(h^(h>>32))&(PS_SPEC_SLOTS-1))==bucket) break;}
        CHECK(i<100000);b=ready(&other);CHECK(a!=b && spec_find(&k)==a && spec_find(&other)==b);
    }
    puts("PASS: complete exact-key identity and queued/claimed/ready/failed state publication on repeated lookups");
}
static void test_full(void)
{
    unsigned i;PsKey k;
    for(i=0;i<PS_SPEC_SLOTS;i++) {k=number(i);ready(&k);}
    k=number(17);PsSpec *old=spec_find(&k);CHECK(old && spec_find(&k)==old);
    k=number(PS_SPEC_SLOTS+1);CHECK(!spec_find(&k) && !spec_request(&k,0));
    for(i=0;i<PS_SPEC_SLOTS;i++) {k=number(i);CHECK(spec_find(&k) && !memcmp(&spec_find(&k)->key,&k,sizeof k));}
    puts("PASS: actual 4096-entry table saturation returns NULL for missing keys and retains every existing key");
}
static void test_queue(void)
{
    PsKey k=number(1);PsSpec *e;
    s_spec_head=PS_SPEC_QUEUE;s_spec_tail=0;
    CHECK(!spec_request(&k,0));e=spec_find(&k);CHECK(e && e->state==0);
    CHECK(!spec_request(&k,0) && spec_find(&k)->state==0);
    s_spec_tail=1;CHECK(!spec_request(&k,0));CHECK(e->state==1);
    CHECK(s_spec_q[0]==e && s_spec_head==PS_SPEC_QUEUE+1);
    /* Prewarmed queued work must still promote to the live queue. */
    k=number(2);CHECK(!spec_request(&k,1));e=spec_find(&k);CHECK(e->prewarm && !e->promoted);
    CHECK(!spec_request(&k,0) && !e->promoted); /* Live queue is full. */
    s_spec_tail=2;CHECK(!spec_request(&k,0));CHECK(e->promoted && s_spec_q[1]==e);
    CHECK(!spec_request(&k,0) && s_spec_head==PS_SPEC_QUEUE+2);
    /* Reinitialize like standalone scheduler tests do: an old front pointer
     * must never survive a changed allocation or cache a fresh EMPTY slot. */
    k=number(999);ready(&k);CHECK(spec_find(&k)->state==2);
    DeleteCriticalSection(&s_spec_cs);free(s_spec);fixture();
    e=spec_find(&k);CHECK(e && e->state==0);CHECK(!spec_request(&k,0) && e->state==1);
    puts("PASS: queue-full EMPTY lookup remains retryable, warm live promotion survives full queue and repeated requests deduplicate");
}

typedef PsSpec *(*Lookup)(const PsKey *);
static PsSpec *table,*last;
static uint32_t fnv(const PsKey *key)
{
    uint32_t h=2166136261u;unsigned i;const uint8_t *p=(const uint8_t *)key;
    for(i=0;i<sizeof *key;i++) h=(h^p[i])*16777619u;return h;
}
static uint32_t lanes(const PsKey *key)
{uint64_t h=hash_mem((const uint8_t *)key,sizeof *key);return (uint32_t)(h^(h>>32));}
static PsSpec *find_hash(const PsKey *key,int four_lanes)
{
    uint32_t h=four_lanes?lanes(key):fnv(key),i;
    for(i=0;i<PS_SPEC_SLOTS;i++) {
        PsSpec *e=&table[(h+i)&(PS_SPEC_SLOTS-1)];
        if(!InterlockedCompareExchange(&e->state,0,0)||!memcmp(&e->key,key,sizeof *key)) return e;
    }
    return NULL;
}
static __declspec(noinline) PsSpec *baseline(const PsKey *key) {return find_hash(key,0);}
static __declspec(noinline) PsSpec *four_lane(const PsKey *key) {return find_hash(key,1);}
static PsSpec *find_front(const PsKey *key,int four_lanes)
{
    PsSpec *e;
    if(last && InterlockedCompareExchange(&last->state,0,0) && !memcmp(&last->key,key,sizeof *key)) return last;
    e=find_hash(key,four_lanes);
    if(e && InterlockedCompareExchange(&e->state,0,0)) last=e;
    return e;
}
static __declspec(noinline) PsSpec *front(const PsKey *key) {return find_front(key,0);}
static __declspec(noinline) PsSpec *combined(const PsKey *key) {return find_front(key,1);}
static __declspec(noinline) PsSpec *production(const PsKey *key) {return spec_find(key);}
static volatile uintptr_t sink;
static __declspec(noinline) double measure(Lookup lookup,const PsKey *keys,unsigned count,unsigned batch)
{
    LARGE_INTEGER f;int64_t start;unsigned i;const unsigned iterations=200000;
    QueryPerformanceFrequency(&f);start=qpc();
    for(i=0;i<iterations;i++) sink^=(uintptr_t)lookup(&keys[(i/batch)%count]);
    return (qpc()-start)*1e9/f.QuadPart/iterations;
}
static void benchmark(const char *cache)
{
    wchar_t path[NV_SHADER_CACHE_PATH];uint8_t family[32];
    uint32_t records[NV_PS_MANIFEST_LIMIT][NV_PS_MANIFEST_WORDS];
    PsKey keys[NV_PS_MANIFEST_LIMIT];unsigned count=0,i,kind,pattern,sample,j;
    PsSpec *tables[5];Lookup lookup[5]={baseline,front,four_lane,combined,production};
    const char *names[5]={"FNV","FNV+front","four-lane","four-lane+front","production"};
    double timings[5][9];
    if(cache) CHECK(!_putenv_s("RECOMP_SHADER_CACHE_DIR",cache));
    _putenv_s("RECOMP_SHADER_CACHE","1");spec_family(family);
    CHECK(nv_ps_manifest_path(family,path) && nv_ps_manifest_read(path,family,records,&count) && count);
    for(i=0;i<count;i++) {CHECK(records[i][41]<=3);memcpy(&keys[i],records[i],sizeof(PsKey));}
    printf("LOOKUP actual validated manifest: %u keys, %zu-byte keys, %u slots, schema %u; source/family/SHA/length validated\n",
           count,sizeof(PsKey),PS_SPEC_SLOTS,NV_PS_MANIFEST_SCHEMA);
    for(kind=0;kind<5;kind++) {
        table=s_spec=tables[kind]=calloc(PS_SPEC_SLOTS,sizeof(PsSpec));CHECK(table);last=NULL;
        for(i=0;i<count;i++) {PsSpec *e=lookup[kind](&keys[i]);CHECK(e);e->key=keys[i];InterlockedExchange(&e->state,2);}
        for(i=0;i<count;i++) CHECK(!memcmp(&lookup[kind](&keys[i])->key,&keys[i],sizeof(PsKey)));
    }
    for(pattern=0;pattern<3;pattern++) {
        unsigned n=pattern==0?1:count,batch=pattern==1?4:1;
        for(sample=0;sample<9;sample++) for(j=0;j<5;j++) {
            kind=(sample+j)%5;table=s_spec=tables[kind];last=NULL;
            timings[kind][sample]=measure(lookup[kind],keys,n,batch);
        }
        printf("LOOKUP pattern=%s, median of 9 x 200000 calls:\n",pattern==0?"one repeated key":pattern==1?"4 draws/key":"all keys alternating");
        for(kind=0;kind<5;kind++) {
            for(i=0;i<9;i++) for(j=i+1;j<9;j++) if(timings[kind][j]<timings[kind][i]) {
                double t=timings[kind][i];timings[kind][i]=timings[kind][j];timings[kind][j]=t;
            }
            printf("  %-16s %.2f ns/call, %.3f ms total, %.3f ms/850 draws\n",names[kind],timings[kind][4],timings[kind][4]*.2,timings[kind][4]*850/1e6);
        }
    }
    for(kind=0;kind<5;kind++) free(tables[kind]);s_spec=NULL;
}
int main(int argc,char **argv)
{
    CHECK(argc>=2);
    if(!strcmp(argv[1],"benchmark")) {benchmark(argc>2?argv[2]:NULL);return 0;}
    fixture();
    if(!strcmp(argv[1],"lookup")) test_lookup();
    else if(!strcmp(argv[1],"full")) test_full();
    else if(!strcmp(argv[1],"queue")) test_queue();
    else CHECK(0);
    return 0;
}

/* These renderer-only fixtures never request executor frame replay. */
void nv2a_pb_interp_enable(int on) { (void)on; abort(); }
long nv2a_pb_frames_done(void) { abort(); return 0; }
int nv2a_pb_replay(float t) { (void)t; abort(); return 0; }
