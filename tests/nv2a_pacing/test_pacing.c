/* Exercise the production helper with deterministic Win32 failure injection
 * and, separately, actual native waits. No GPU/context is needed. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static int mocked = 1;
static int64_t clock_ticks, test_frequency = 1000000;
static int64_t armed_units, wake_extra, sleep_ticks = 1000;
static unsigned create_calls, arm_calls, wait_calls, sleep_calls, spin_calls;
static DWORD create_flags[2];
static unsigned fail_creates;
static int fail_arm;
static DWORD wait_result = WAIT_OBJECT_0;

static HANDLE pacing_create(LPSECURITY_ATTRIBUTES sa, LPCWSTR name, DWORD flags, DWORD access)
{
    if (!mocked) return CreateWaitableTimerExW(sa,name,flags,access);
    CHECK(!sa && !name && access == TIMER_ALL_ACCESS);
    CHECK(create_calls < 2);create_flags[create_calls++] = flags;
    if (fail_creates) { fail_creates--;SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL; }
    return (HANDLE)(uintptr_t)1;
}
static BOOL pacing_arm(HANDLE timer,const LARGE_INTEGER *due,LONG period,
                        PTIMERAPCROUTINE completion,LPVOID arg,BOOL resume)
{
    if (!mocked) return SetWaitableTimer(timer,due,period,completion,arg,resume);
    CHECK(timer == (HANDLE)(uintptr_t)1 && due->QuadPart < 0);
    CHECK(!period && !completion && !arg && !resume);
    arm_calls++;armed_units = -due->QuadPart;
    if(fail_arm) { SetLastError(ERROR_INVALID_HANDLE);return FALSE; }
    return TRUE;
}
static DWORD pacing_wait(HANDLE timer,DWORD timeout)
{
    if (!mocked) return WaitForSingleObject(timer,timeout);
    CHECK(timer == (HANDLE)(uintptr_t)1 && timeout == INFINITE);wait_calls++;
    if(wait_result == WAIT_OBJECT_0) clock_ticks += armed_units * test_frequency / 10000000 + wake_extra;
    else SetLastError(ERROR_INVALID_HANDLE);
    return wait_result;
}
static BOOL pacing_clock(LARGE_INTEGER *out)
{
    if(!mocked) return QueryPerformanceCounter(out);
    out->QuadPart=clock_ticks;return TRUE;
}
static void pacing_sleep(DWORD ms)
{
    if(!mocked) { Sleep(ms);return; }
    CHECK(ms == 1);sleep_calls++;clock_ticks += sleep_ticks;
}
static void pacing_yield(void)
{
    if(!mocked) { YieldProcessor();return; }
    spin_calls++;clock_ticks += 10;
}

#define CreateWaitableTimerExW pacing_create
#define SetWaitableTimer pacing_arm
#define WaitForSingleObject pacing_wait
#define QueryPerformanceCounter pacing_clock
#define Sleep pacing_sleep
#undef YieldProcessor
#define YieldProcessor pacing_yield
#include "nv2a_pacing.h"
#undef CreateWaitableTimerExW
#undef SetWaitableTimer
#undef WaitForSingleObject
#undef QueryPerformanceCounter
#undef Sleep
#undef YieldProcessor

static void reset(void)
{
    mocked=1;clock_ticks=1000000;test_frequency=1000000;
    armed_units=0;wake_extra=0;sleep_ticks=1000;
    create_calls=arm_calls=wait_calls=sleep_calls=spin_calls=0;
    fail_creates=0;fail_arm=0;wait_result=WAIT_OBJECT_0;
    memset(create_flags,0,sizeof create_flags);
}
static int old_deadline(int64_t *next,int64_t now,int64_t frequency,int limit)
{
    if(limit<=0)return 0;
    *next += frequency/limit;
    if(!*next || *next <= now) { *next=now;return 0; }
    return 1;
}
static void test_cadence(void)
{
    static const int rates[]={1,30,60,120,144,1000};
    unsigned r,i;int64_t next,old,now;
    reset();
    next=123;CHECK(!nv_pacing_deadline(&next,1000,1000000,0) && next==123);
    CHECK(!nv_pacing_deadline(&next,1000,1000000,-3) && next==123);
    CHECK(!nv_pacing_deadline(&next,1000,0,60) && next==123);
    for(r=0;r<sizeof rates/sizeof rates[0];r++) {
        next=old=0;now=100000000;
        for(i=0;i<10000;i++) {
            int actual=nv_pacing_deadline(&next,now,1000000,rates[r]);
            int expected=old_deadline(&old,now,1000000,rates[r]);
            CHECK(actual==expected && next==old);
            /* Frames can be early, exactly at a boundary, or several periods
             * late. A late frame is returned immediately, never double-paced. */
            if(actual && i%11) now=next+((int)(i%3)-1)*10;
            else now+=1000000/rates[r]*(i%7+1);
        }
    }
    next=0;CHECK(!nv_pacing_deadline(&next,1000,1000,2000) && next==1000);
    next=INT64_MAX-10;CHECK(!nv_pacing_deadline(&next,INT64_MAX-5,1000000,60));
    CHECK(next==INT64_MAX-5);
    next=10;CHECK(!nv_pacing_deadline(&next,-1,1000000,60) && next==-1);
    next=0;CHECK(nv_pacing_deadline(&next,0,1000000,60) && next==16666);
    CHECK(!create_calls && !arm_calls && !wait_calls && !sleep_calls && !spin_calls);
    puts("PASS: 60000 legacy deadline/cadence comparisons, off/negative/zero-rate, first/late/exact-boundary frames and signed overflow reset");
}
static void check_fallback(HANDLE timer)
{
    int64_t start=clock_ticks,deadline=start+16666;
    nv_pacing_wait(timer,test_frequency,deadline,start);
    CHECK(clock_ticks>=deadline && sleep_calls>0);
    CHECK(spin_calls*10<=test_frequency/1000+10);
    CHECK(clock_ticks-deadline<sleep_ticks+10);
}
static void test_failures(void)
{
    HANDLE timer;int64_t start,deadline;
    reset();timer=nv_pacing_create_timer();
    CHECK(timer && create_calls==1 && create_flags[0]==CREATE_WAITABLE_TIMER_HIGH_RESOLUTION);
    start=clock_ticks;deadline=start+16666;
    nv_pacing_wait(timer,test_frequency,deadline,start);
    CHECK(arm_calls==1 && wait_calls==1 && sleep_calls==0 && spin_calls==100);
    CHECK(armed_units==(16666-1000)*10 && clock_ticks==deadline);
    reset();fail_creates=1;timer=nv_pacing_create_timer();
    CHECK(timer && create_calls==2 && create_flags[0]==CREATE_WAITABLE_TIMER_HIGH_RESOLUTION && !create_flags[1]);
    start=clock_ticks;nv_pacing_wait(timer,test_frequency,start+16666,start);
    CHECK(arm_calls==1 && wait_calls==1 && !sleep_calls);
    reset();fail_creates=2;timer=nv_pacing_create_timer();CHECK(!timer && create_calls==2);
    check_fallback(timer);CHECK(!arm_calls && !wait_calls);
    reset();fail_arm=1;check_fallback((HANDLE)(uintptr_t)1);CHECK(arm_calls==1 && !wait_calls);
    reset();wait_result=WAIT_FAILED;check_fallback((HANDLE)(uintptr_t)1);CHECK(arm_calls==1 && wait_calls==1);
    reset();wait_result=WAIT_TIMEOUT;check_fallback((HANDLE)(uintptr_t)1);CHECK(sleep_calls>0);
    reset();sleep_ticks=16000;check_fallback(NULL);CHECK(sleep_calls==1 && spin_calls<=100);
    reset();sleep_ticks=17000;check_fallback(NULL);CHECK(sleep_calls==1 && !spin_calls);
    /* A transient arm failure may recover on the next frame. Successful
     * waits keep the original timer call and one-ms tail, without sleeps. */
    reset();fail_arm=1;check_fallback((HANDLE)(uintptr_t)1);
    fail_arm=0;sleep_calls=spin_calls=0;start=clock_ticks;
    nv_pacing_wait((HANDLE)(uintptr_t)1,test_frequency,start+16666,start);
    CHECK(!sleep_calls && spin_calls==100);
    reset();start=clock_ticks;nv_pacing_wait(NULL,test_frequency,start+500,start);
    CHECK(!arm_calls && !wait_calls && !sleep_calls && spin_calls==50);
    reset();wake_extra=5000;start=clock_ticks;
    nv_pacing_wait((HANDLE)(uintptr_t)1,test_frequency,start+16666,start);
    CHECK(clock_ticks>=start+16666 && !sleep_calls && !spin_calls);
    puts("PASS: high-res/ordinary/create-both-fail/arm/wait failures, bounded one-ms spin and Sleep requests, coarse/late wake, successful retry and unchanged successful timer tail");
}
static void test_conversion(void)
{
    static const int64_t frequencies[]={60,1000,1000000,10000000,3579545,3000000000ll};
    unsigned i,j;
    for(i=0;i<sizeof frequencies/sizeof frequencies[0];i++)
        for(j=1;j<=10000;j++) {
            int64_t t=frequencies[i]*j/10000;
            int64_t expected=t*10000000/frequencies[i];
            if(expected<1)expected=1;
            CHECK(nv_pacing_timer_units(t,frequencies[i])==expected);
        }
    CHECK(nv_pacing_timer_units(INT64_MAX,INT64_MAX)==10000000);
    CHECK(nv_pacing_timer_units(INT64_MAX/2,INT64_MAX)==4999999);
    CHECK(nv_pacing_timer_units(INT64_MAX-1,INT64_MAX)==9999999);
    CHECK(nv_pacing_timer_units(INT64_MAX,1)==INT64_MAX);
    CHECK(nv_pacing_timer_units(INT64_MAX-1,2)==INT64_MAX);
    CHECK(nv_pacing_timer_units(1,INT64_MAX)==1);
    puts("PASS: 60000 exact relative-unit conversions, sub-100ns clamp and overflowing products/division/saturation");
}
static void old_wait(HANDLE timer,int64_t frequency,int64_t deadline,int64_t now)
{
    int64_t remain=deadline-now,spin=frequency/1000;
    if(remain>spin && timer) {
        LARGE_INTEGER due;due.QuadPart=-(remain-spin)*10000000/frequency;
        CHECK(SetWaitableTimer(timer,&due,0,NULL,NULL,FALSE));
        CHECK(WaitForSingleObject(timer,INFINITE)==WAIT_OBJECT_0);
    }
    while(nv_pacing_qpc()<deadline)pacing_yield();
}
static void test_native(void)
{
    LARGE_INTEGER f;HANDLE timer;int64_t next=0,now,expected;unsigned i,legacy;
    mocked=0;CHECK(QueryPerformanceFrequency(&f));timer=nv_pacing_create_timer();CHECK(timer);
    for(legacy=0;legacy<2;legacy++) {
        next=0;
        for(i=0;i<30;i++) {
            now=nv_pacing_qpc();expected=next;
            CHECK(nv_pacing_deadline(&next,now,f.QuadPart,60)==old_deadline(&expected,now,f.QuadPart,60));
            CHECK(next==expected);
            if(next<=now)continue;
            if(legacy)old_wait(timer,f.QuadPart,next,now);
            else nv_pacing_wait(timer,f.QuadPart,next,now);
            CHECK(nv_pacing_qpc()>=next);
        }
    }
    CloseHandle(timer);
    puts("PASS: actual Windows helper and original timer paths retain identical cadence calculations and never finish before their native QPC deadline");
}
int main(int argc,char **argv)
{
    CHECK(argc==2);
    if(!strcmp(argv[1],"cadence"))test_cadence();
    else if(!strcmp(argv[1],"failures"))test_failures();
    else if(!strcmp(argv[1],"conversion"))test_conversion();
    else if(!strcmp(argv[1],"native"))test_native();
    else CHECK(0);
    return 0;
}
