#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int mock,query_count,xgetbv_count,max_leaf;
static uint32_t cpuid1,cpuid7;
static uint64_t xcr0;
static void cpu_query(int result[4],int leaf,int subleaf)
{
    query_count++;
    if(!mock){__cpuidex(result,leaf,subleaf);return;}
    memset(result,0,4*sizeof(int));
    if(leaf==0)result[0]=max_leaf;
    if(leaf==1)result[2]=(int)cpuid1;
    if(leaf==7)result[1]=(int)cpuid7;
}
static uint64_t os_state(unsigned index)
{xgetbv_count++;return mock?xcr0:_xgetbv(index);}
#define NV_HASH_CPUID cpu_query
#define NV_HASH_XGETBV os_state
#include "nv2a_texture_hash.h"
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)

/* Original scalar implementation, kept independent of the dispatcher. */
static __declspec(noinline) uint64_t scalar(const uint8_t *p,size_t n)
{
    uint64_t a=0x9E3779B97F4A7C15ull,b=0xC2B2AE3D27D4EB4Full,
             c=0x165667B19E3779F9ull,d=0x27D4EB2F165667C5ull^n;
    size_t i=0;
    for(;i+32<=n;i+=32) {uint64_t w[4];memcpy(w,p+i,32);
        a=(a^w[0])*0x100000001B3ull;a^=a>>31;
        b=(b^w[1])*0x100000001B3ull;b^=b>>31;
        c=(c^w[2])*0x100000001B3ull;c^=c>>31;
        d=(d^w[3])*0x100000001B3ull;d^=d>>31;
    }
    for(;i<n;i++)a=(a^p[i])*0x100000001B3ull;
    return a^(b*3)^(c*5)^(d*7);
}
static int native_supported;
static void check_hash(const uint8_t *p,size_t bytes)
{
    uint64_t old=scalar(p,bytes);CHECK(hash_mem(p,bytes)==old);
    if(native_supported)CHECK(hash_mem_native(p,bytes)==old);
}
static void fill(uint8_t *p,size_t n)
{
    uint32_t x=0x12345678;size_t i;
    for(i=0;i<n;i++){x^=x<<13;x^=x>>17;x^=x<<5;p[i]=(uint8_t)x;}
}
static void exact(void)
{
    uint8_t *data=malloc(8*1024*1024+64);unsigned i,j,pattern;
    const size_t sizes[]={65535,65536,65537,1048575,1048576,1048577,8388608};
    CHECK(data);fill(data,8*1024*1024+64);
    for(j=0;j<64;j++)for(i=0;i<8192;i++)check_hash(data+j,i);
    for(i=0;i<sizeof sizes/sizeof sizes[0];i++)for(j=0;j<64;j++)check_hash(data+j,sizes[i]);
    for(pattern=0;pattern<2;pattern++){
        memset(data,pattern?255:0,8192+64);
        for(j=0;j<64;j++)for(i=0;i<8192;i+=31)check_hash(data+j,i);
    }
    free(data);
    puts("PASS: 524288 exact short-length/alignment cases,448 large cases and33920 zero/ones cases; original scalar,production dispatch and supported native path");
}
static void pages(void)
{
    uint8_t *base=VirtualAlloc(NULL,65536+8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    uint8_t *data;DWORD old;unsigned n,i;CHECK(base);data=base+4096;
    CHECK(VirtualProtect(base,4096,PAGE_NOACCESS,&old));
    CHECK(VirtualProtect(data+65536,4096,PAGE_NOACCESS,&old));fill(data,65536);
    for(n=0;n<=8192;n++)check_hash(data+65536-n,n);
    check_hash(data,65536);
    for(i=0;i<32;i++)check_hash(data+i,4096+i);
    VirtualFree(base,0,MEM_RELEASE);
    puts("PASS: 8226 exact protected-page cases, no read beyond an inaccessible following/preceding page, all tails and empty input");
}
static void supported_mock(void)
{mock=1;max_leaf=7;cpuid1=0x1C000000;cpuid7=0x80030020;xcr0=0xE6;query_count=xgetbv_count=0;}
static void features(void)
{
    unsigned i;const unsigned bits1[]={26,27,28},bits7[]={5,16,17,31},bitsx[]={1,2,5,6,7};
    uint8_t data[4096];fill(data,sizeof data);supported_mock();CHECK(hash_mem_cpu_native());
    CHECK(xgetbv_count==1);supported_mock();max_leaf=6;s_hash_mem_native=-1;
    CHECK(hash_mem(data,sizeof data)==scalar(data,sizeof data) && s_hash_mem_native==0 && !xgetbv_count);
    for(i=0;i<3;i++){
        supported_mock();cpuid1&=~(1u<<bits1[i]);s_hash_mem_native=-1;
        CHECK(hash_mem(data,sizeof data)==scalar(data,sizeof data) && s_hash_mem_native==0 && !xgetbv_count);
    }
    for(i=0;i<4;i++){
        supported_mock();cpuid7&=~(1u<<bits7[i]);s_hash_mem_native=-1;
        CHECK(hash_mem(data,sizeof data)==scalar(data,sizeof data) && s_hash_mem_native==0 && xgetbv_count==1);
    }
    for(i=0;i<5;i++){
        supported_mock();xcr0&=~(1ull<<bitsx[i]);s_hash_mem_native=-1;
        CHECK(hash_mem(data,sizeof data)==scalar(data,sizeof data) && s_hash_mem_native==0 && xgetbv_count==1);
    }
    /* Force real production fallback with mocked OSXSAVE absence. No XGETBV
     * or feature instruction is allowed, even when hashing a large buffer. */
    supported_mock();cpuid1&=~(1u<<27);s_hash_mem_native=-1;
    CHECK(hash_mem(data,sizeof data)==scalar(data,sizeof data));
    CHECK(s_hash_mem_native==0 && query_count==2 && xgetbv_count==0);
    for(i=0;i<32;i++)CHECK(hash_mem(data,sizeof data)==scalar(data,sizeof data));
    CHECK(query_count==2 && xgetbv_count==0); /* Detection is cached. */
    supported_mock();s_hash_mem_native=-1;CHECK(hash_mem(data,216)==scalar(data,216));
    CHECK(s_hash_mem_native==-1 && query_count==0 && xgetbv_count==0); /* Shader keys stay scalar. */
    mock=0;s_hash_mem_native=-1;CHECK(hash_mem(data,sizeof data)==scalar(data,sizeof data));
    CHECK(s_hash_mem_native==native_supported);
    puts("PASS: every required CPU/OS bit absence, missing leaf, cached scalar fallback without XGETBV,216-byte short path without feature queries and real native/scalar selection");
}
typedef uint64_t (*Hash)(const uint8_t *,size_t);
static volatile uint64_t sink;
static __declspec(noinline) uint64_t production(const uint8_t *p,size_t n){return hash_mem(p,n);}
uint64_t threshold_1024(const uint8_t *p,size_t n);
uint64_t threshold_2048(const uint8_t *p,size_t n);
uint64_t threshold_4096(const uint8_t *p,size_t n);
static __declspec(noinline) double measure(Hash hash,uint8_t *p,size_t n,unsigned count)
{
    LARGE_INTEGER f,a,b;unsigned i;QueryPerformanceFrequency(&f);QueryPerformanceCounter(&a);
    for(i=0;i<count;i++){p[0]++;sink^=hash(p,n);}QueryPerformanceCounter(&b);
    return (b.QuadPart-a.QuadPart)*1e9/f.QuadPart/count;
}
static void benchmark(void)
{
    uint8_t *data=malloc(8*1024*1024+64);unsigned item,kind,sample,i,j;
    const size_t lengths[]={216,512,1024,2048,4096,8192,65536,1048576,8388608};
    Hash hashes[5]={scalar,threshold_1024,threshold_2048,threshold_4096,production};
    const char *names[5]={"old scalar","dispatch>=1024","dispatch>=2048","dispatch>=4096","production"};
    CHECK(data);fill(data,8*1024*1024+64);hash_mem(data,8192);
    printf("HASH real native feature=%d, production threshold=%u; generic compiler ISA, cached CPUID/OS dispatch+noinline callee included\n",native_supported,NV_HASH_NATIVE_MIN);
    for(item=0;item<sizeof lengths/sizeof lengths[0];item++){
        size_t n=lengths[item];unsigned count=(unsigned)(32*1024*1024/n);double values[5][9];if(count<8)count=8;
        for(sample=0;sample<9;sample++)for(i=0;i<5;i++){kind=(sample+i)%5;values[kind][sample]=measure(hashes[kind],data+1,n,count);}
        printf("HASH %zu bytes,9x%u calls,unaligned source:\n",n,count);
        for(kind=0;kind<5;kind++){
            for(i=0;i<9;i++)for(j=i+1;j<9;j++)if(values[kind][j]<values[kind][i]){double t=values[kind][i];values[kind][i]=values[kind][j];values[kind][j]=t;}
            printf("  %-17s %10.2f ns, %8.3f ms total\n",names[kind],values[kind][4],values[kind][4]*count/1e6);
        }
    }
    free(data);
}
int main(int argc,char **argv)
{
    CHECK(argc==2);native_supported=hash_mem_cpu_native();
    printf("Native feature available=%d\n",native_supported);
    if(!strcmp(argv[1],"exact"))exact();
    else if(!strcmp(argv[1],"pages"))pages();
    else if(!strcmp(argv[1],"features"))features();
    else if(!strcmp(argv[1],"benchmark"))benchmark();
    else CHECK(0);
    return 0;
}
