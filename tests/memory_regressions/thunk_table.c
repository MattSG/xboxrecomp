/* Synthetic XBE import-table selection, bounds and exact patch ownership. */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void *recomp_lookup(ULONG address) { (void)address; return NULL; }
void *recomp_lookup_manual(ULONG address) { (void)address; return NULL; }
static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "thunk line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static void put(uint8_t *bytes, unsigned offset, uint32_t value)
{ memcpy(bytes + offset, &value, 4); }
static uint32_t *word(uint32_t va)
{ return (uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + va); }
struct stack_request { HANDLE ready,release; uint32_t top; };
static DWORD WINAPI allocate_worker_stack(LPVOID opaque)
{
    struct stack_request *request=opaque;
    request->top=xbox_AllocThreadStackSized(8192);
    SetEvent(request->ready);
    WaitForSingleObject(request->release,5000);
    xbox_FreeThreadStack(request->top);
    return request->top ? 0 : 1;
}
int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "high-retail";
    int kdata_config = !strcmp(mode,"kernel-data-config");
    int kdata_invalid = !strcmp(mode,"kernel-data-header") || !strcmp(mode,"kernel-data-overlap") || !strcmp(mode,"kernel-data-unbacked");
    int high = !strcmp(mode, "high-retail") || !strcmp(mode, "high-debug");
    int invalid = !strcmp(mode,"thread-stack-unbacked") || kdata_invalid || !strcmp(mode,"partial") || !strcmp(mode,"unterminated") ||
                  !strcmp(mode,"bad-ordinal") || !strcmp(mode,"bad-marker");
    uint32_t data_va = !strcmp(mode,"kernel-data-auto") ? 0x740000u : high ? 0x04010000u : 0x30000u;
    uint32_t table_va = data_va + 64, actual_base, actual_count;
    uint32_t expected = 2;
    uint8_t *xbe = calloc(1, 0x3000);
    if (!xbe) return 2;
    memcpy(xbe, "XBEH", 4);
    put(xbe,0x104,0x10000); put(xbe,0x108,0x1000);
    put(xbe,0x11c,2); put(xbe,0x120,0x10200); put(xbe,0x130,4096);
    put(xbe,0x200,4); put(xbe,0x204,0x20000); put(xbe,0x208,4096);
    put(xbe,0x20c,0x1000); put(xbe,0x210,4096);
    put(xbe,0x238,2); put(xbe,0x23c,data_va); put(xbe,0x240,4096);
    put(xbe,0x244,0x2000); put(xbe,0x248,4096);
    if (!strcmp(mode,"kernel-data-auto")) put(xbe,0x2000,0xA5A5A5A5u);
    put(xbe,0x203c,0x80000067); /* data outside the selected table */
    put(xbe,0x2040,0x80000067); put(xbe,0x2044,0x80000097);
    if (!strcmp(mode,"empty")) { put(xbe,0x2040,0); expected=0; }
    if (!strcmp(mode,"none") || !strcmp(mode,"encoded-none")) expected=0;
    if (!strcmp(mode,"partial")) table_va=0x08000000;
    if (!strcmp(mode,"bad-ordinal")) put(xbe,0x2040,0x80000000u);
    if (!strcmp(mode,"remapped") || !strcmp(mode,"remapped-data")) put(xbe,0x2040,0x800001ff);
    if (!strcmp(mode,"bad-marker")) put(xbe,0x2040,103);
    if (!strcmp(mode,"unterminated") || !strcmp(mode,"maximum")) {
        for (unsigned i=0;i<XBOX_KERNEL_THUNK_TABLE_SIZE+1;i++) put(xbe,0x2040+i*4,0x80000067);
        if (!strcmp(mode,"maximum")) { put(xbe,0x2040+XBOX_KERNEL_THUNK_TABLE_SIZE*4,0); expected=XBOX_KERNEL_THUNK_TABLE_SIZE; }
    }
    if (!strcmp(mode,"sparse")) { put(xbe,0x2044,0); put(xbe,0x2048,0x80000097); expected=1; }
    put(xbe,0x158,!strcmp(mode,"none") ? 0 : table_va ^
        (!strcmp(mode,"high-debug") ? 0xEFB1F152u : 0x5B6D40B6u));
    if (!strcmp(mode,"encoded-none")) put(xbe,0x158,0x5B6D40B6u);
    if (kdata_config || kdata_invalid) {
        uint32_t selected = kdata_config ? 0x500000u :
                            !strcmp(mode,"kernel-data-overlap") ? 0x20000u :
                            !strcmp(mode,"kernel-data-header") ? 0x10000u : 0x8000000u;
        CHECK(xbox_ConfigureKernelData(selected));
        CHECK(!xbox_ConfigureKernelData(0x500001u));
        CHECK(!xbox_ConfigureKernelData(0x1000u));
        CHECK(!xbox_ConfigureKernelData(0xFFFFF000u));
        CHECK(XBOX_KERNEL_DATA_BASE == selected);
    }
    if (!strcmp(mode,"kernel-data-empty")) {
        put(xbe,0x11c,0); put(xbe,0x158,0); expected=0;
    }
    if (!strcmp(mode,"thread-stack-policy") || !strcmp(mode,"kernel-stacks")) {
        CHECK(xbox_ConfigureThreadStack(256*1024));
        CHECK(!xbox_ConfigureThreadStack(123));
    }
    if (!strcmp(mode,"thread-stack-unbacked")) CHECK(xbox_ConfigureThreadStack(0x08001000u));
    xbox_SetTotalRam(XBOX_DEVKIT_RAM);
    xbox_kernel_set_thunk_address(0x60000,3); /* previous image metadata */
    BOOL loaded = xbox_MemoryLayoutInit(xbe,0x3000);
    CHECK(loaded == !invalid);
    xbox_kernel_get_thunk_address(&actual_base,&actual_count);
    if (invalid) {
        CHECK(!xbox_GetMemoryBase() && !actual_base && !actual_count);
        CHECK(!XBOX_KERNEL_DATA_BASE);
        goto done;
    }
    if (!loaded) goto done;
    CHECK(actual_base == (expected ? table_va : 0) && actual_count == expected);
    if (!strcmp(mode,"kernel-data-empty")) CHECK(XBOX_KERNEL_DATA_BASE == 0x11000u);
    if (!strcmp(mode,"kernel-data-auto") || kdata_config) {
        uint32_t original = *word(data_va);
        CHECK(!xbox_ConfigureKernelData(0));
        if (kdata_config) CHECK(XBOX_KERNEL_DATA_BASE == 0x500000u);
        *word(table_va) = 0x80000010u; /* relocated ExEventObjectType */
        xbox_kernel_init();
        xbox_kernel_bridge_init();
        CHECK(XBOX_KERNEL_DATA_BASE >= g_xbox_image_hi);
        CHECK(!(XBOX_KERNEL_DATA_BASE & 4095));
        CHECK(*word(table_va) == XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE);
        CHECK(*word(data_va) == original);
        CHECK(XBOX_STACK_BASE >= XBOX_KERNEL_DATA_BASE + XBOX_KERNEL_DATA_SIZE);
        CHECK(xbox_HeapAlloc(4096,4096) >= XBOX_WORKER_STACK_END);
        xbox_kernel_shutdown();
    }
    if (!strcmp(mode,"kernel-stacks")) {
        uint32_t high=xbox_AllocKernelStack(801),low=0,end=0;
        CHECK(high && xbox_GetStackBounds(high-1,&low,&end));
        CHECK(end==high && high-low==801);
        CHECK(!xbox_GetStackBounds(high,NULL,NULL));
        xbox_FreeThreadStack(high-16); CHECK(xbox_GetStackBounds(high-1,NULL,NULL));
        xbox_FreeKernelStack(high,low+16); CHECK(xbox_GetStackBounds(high-1,NULL,NULL));
        xbox_FreeKernelStack(high,low); CHECK(!xbox_GetStackBounds(high-1,NULL,NULL));
        xbox_FreeKernelStack(high,low);
        uint32_t worker=xbox_AllocThreadStack();
        CHECK(xbox_GetStackBounds(worker,&low,&end));
        xbox_FreeKernelStack(end,low); CHECK(xbox_GetStackBounds(worker,NULL,NULL));
        xbox_FreeThreadStack(worker);
        uint32_t ordinary=xbox_HeapAlloc(801,16);
        xbox_FreeKernelStack(ordinary+801,ordinary); CHECK(xbox_HeapBlockSize(ordinary)>=801);
        xbox_HeapFree(ordinary);
        CHECK(!xbox_AllocKernelStack(0) && !xbox_AllocKernelStack(UINT32_MAX));
    }
    if (!strcmp(mode,"stack-bounds")) {
        uint32_t low=1,high=1,top=xbox_AllocThreadStackSized(8192);
        CHECK(xbox_GetStackBounds(XBOX_STACK_TOP,&low,&high));
        CHECK(low==XBOX_STACK_BASE && high==XBOX_STACK_BASE+XBOX_STACK_SIZE);
        CHECK(xbox_GetStackBounds(top,&low,&high) && high-low==8192 && high==top+16);
        CHECK(xbox_GetStackBounds(low,NULL,NULL));
        uint32_t worker_low=low,worker_high=high;
        CHECK(!xbox_GetStackBounds(high,NULL,NULL));
        xbox_FreeThreadStack(top);
        CHECK(!xbox_GetStackBounds(worker_low,&low,&high) && !low && !high);
        int lease=xbox_worker_stack_alloc(); CHECK(lease>=0);
        uint32_t callback_top=XBOX_WORKER_STACK_TOP(lease);
        CHECK(xbox_GetStackBounds(callback_top,&low,&high));
        CHECK(high-low==XBOX_WORKER_STACK_SIZE && high==callback_top+16);
        xbox_worker_stack_free(lease);
        CHECK(!xbox_GetStackBounds(callback_top,&low,&high) && !low && !high);
        uint32_t ordinary=xbox_HeapAlloc(8192,4096);
        CHECK(!xbox_GetStackBounds(ordinary,NULL,NULL));
        xbox_HeapFree(ordinary);
        CHECK(!xbox_GetStackBounds(0,&low,&high) && !low && !high);
        CHECK(!xbox_GetStackBounds(UINT32_MAX,NULL,NULL));
        xbox_MemoryLayoutShutdown();
        CHECK(!xbox_GetStackBounds(worker_high-1,&low,&high) && !low && !high);
        CHECK(xbox_MemoryLayoutInit(xbe,0x3000));
    }
    if (!strcmp(mode,"thread-stack-policy")) {
        CHECK(!xbox_ConfigureThreadStack(0));
        uint32_t a=xbox_AllocThreadStackSized(4097), b=xbox_AllocThreadStackSized(1);
        CHECK(a && b==a+256*1024);
        CHECK(xbox_HeapBlockSize(a)==16 && xbox_HeapBlockSize(b)==16);
        xbox_FreeThreadStack(a+4); CHECK(xbox_HeapBlockSize(a)==16);
        xbox_FreeThreadStack(a); xbox_FreeThreadStack(a); xbox_FreeThreadStack(b);
        a=xbox_AllocThreadStackSized(1024*1024+1); b=xbox_AllocThreadStackSized(1024*1024+1);
        CHECK(a && b==a+1024*1024+4096);
        CHECK(!xbox_AllocThreadStackSized(UINT32_MAX));
        xbox_FreeThreadStack(a); xbox_FreeThreadStack(b);
        xbox_MemoryLayoutShutdown();
        CHECK(!xbox_AllocThreadStack());
        CHECK(xbox_ConfigureThreadStack(0));
        CHECK(xbox_MemoryLayoutInit(xbe,0x3000));
        a=xbox_AllocThreadStack(); b=xbox_AllocThreadStack();
        CHECK(a && b==a+4096);
        xbox_FreeThreadStack(a); xbox_FreeThreadStack(b);
    }
    if (!strcmp(mode,"thread-stack-parallel")) {
        struct stack_request requests[12]={0}; HANDLE threads[12]={0};
        HANDLE release=CreateEventA(NULL,TRUE,FALSE,NULL); CHECK(release!=NULL);
        for (unsigned i=0;i<12;i++) {
            requests[i].ready=CreateEventA(NULL,TRUE,FALSE,NULL); requests[i].release=release;
            threads[i]=CreateThread(NULL,0,allocate_worker_stack,&requests[i],0,NULL);
            CHECK(threads[i]!=NULL);
        }
        for (unsigned i=0;i<12;i++) {
            CHECK(WaitForSingleObject(requests[i].ready,5000)==WAIT_OBJECT_0);
            CHECK(requests[i].top!=0);
            for (unsigned j=0;j<i;j++) CHECK(requests[i].top!=requests[j].top);
        }
        SetEvent(release);
        for (unsigned i=0;i<12;i++) {
            DWORD status=1;
            CHECK(WaitForSingleObject(threads[i],5000)==WAIT_OBJECT_0);
            CHECK(GetExitCodeThread(threads[i],&status) && status==0);
            CHECK(xbox_HeapBlockSize(requests[i].top)==0);
            CloseHandle(threads[i]); CloseHandle(requests[i].ready);
        }
        CloseHandle(release);
    }
    if (!strcmp(mode,"thread-stack-many")) {
        uint32_t tops[12]={0};
        for (unsigned i=0;i<12;i++) {
            tops[i]=xbox_AllocThreadStack(); CHECK(tops[i]!=0);
            for (unsigned j=0;j<i;j++) CHECK(!tops[i] || tops[i]!=tops[j]);
        }
        for (unsigned i=0;i<12;i++) xbox_FreeThreadStack(tops[i]);
        uint32_t ordinary=xbox_HeapAlloc(512*1024,4096);
        CHECK(ordinary!=0);
        xbox_FreeThreadStack(ordinary+512*1024-16);
        CHECK(xbox_HeapBlockSize(ordinary)>=512*1024);
        xbox_HeapFree(ordinary);
    }
    if (!strncmp(mode,"kernel-strings",14)) {
        int empty=!strcmp(mode,"kernel-strings-empty"), invalid_strings=!strcmp(mode,"kernel-strings-invalid");
        int maximum=!strcmp(mode,"kernel-strings-max"), exhausted=!strcmp(mode,"kernel-strings-oom");
        char model[3]={'A',0,'B'}, serial[5]={'1','2','3','4','5'}, path[65535];
        for (unsigned i=0;i<sizeof(path);i++) path[i]=(char)('a'+i%26);
        if (empty) {
            CHECK(!xbox_HalDiskModelNumber.Buffer && !xbox_HalDiskSerialNumber.Buffer && !xbox_XeImageFileName.Buffer);
            CHECK(!xbox_HalDiskCachePartitionCount);
        } else {
            xbox_HalDiskModelNumber=(XBOX_ANSI_STRING){3,3,model};
            xbox_HalDiskSerialNumber=(XBOX_ANSI_STRING){5,5,serial};
            xbox_XeImageFileName=(XBOX_ANSI_STRING){maximum?65535:512,maximum?65535:512,path};
            xbox_HalDiskCachePartitionCount=7;
        }
        if (invalid_strings) {
            xbox_HalDiskModelNumber=(XBOX_ANSI_STRING){1,1,NULL};
            xbox_HalDiskSerialNumber=(XBOX_ANSI_STRING){5,4,serial};
            xbox_XeImageFileName=(XBOX_ANSI_STRING){0,1,path};
        }
        const unsigned ordinals[]={41,42,326};
        const unsigned offsets[]={KDATA_DISK_MODEL_STR,KDATA_DISK_SERIAL_STR,KDATA_XE_IMAGE_FILENAME};
        XBOX_ANSI_STRING *sources[]={&xbox_HalDiskModelNumber,&xbox_HalDiskSerialNumber,&xbox_XeImageFileName};
        for (unsigned i=0;i<3;i++) *word(table_va+i*4)=0x80000000u|ordinals[i];
        xbox_kernel_set_thunk_address(table_va,3);
        xbox_kernel_init();
        if (exhausted) {
            while (xbox_HeapAlloc(1024*1024,4096)) {}
            while (xbox_HeapAlloc(4096,4)) {}
            while (xbox_HeapAlloc(4,4)) {}
        }
        xbox_kernel_bridge_init();
        for (unsigned i=0;i<3;i++) {
            uint32_t descriptor=XBOX_KERNEL_DATA_BASE+offsets[i];
            uint16_t *counts=(uint16_t *)word(descriptor);
            uint32_t buffer=*word(descriptor+4);
            CHECK(xbox_resolve_ordinal(ordinals[i])==(ULONG_PTR)sources[i]);
            CHECK(*word(table_va+i*4)==descriptor);
            if (empty || exhausted || (invalid_strings && i<2)) {
                CHECK(!counts[0] && !counts[1] && !buffer);
            } else {
                CHECK(counts[0]==sources[i]->Length && counts[1]==sources[i]->MaximumLength);
                CHECK(buffer && xbox_HeapBlockSize(buffer)>=sources[i]->MaximumLength);
                if (buffer) {
                    CHECK(!memcmp(word(buffer),sources[i]->Buffer,sources[i]->Length));
                    if (invalid_strings) CHECK(*(uint8_t *)word(buffer)==0);
                }
            }
        }
        CHECK(*word(XBOX_KERNEL_DATA_BASE+KDATA_DISK_CACHE_PARTS)==(empty?0u:7u));
        xbox_kernel_shutdown();
    }
    if (!strcmp(mode,"kernel-identity") || !strcmp(mode,"kernel-identity-default")) {
        int defaults=!strcmp(mode,"kernel-identity-default");
        CHECK(sizeof(xbox_HardwareInfo)==8 && sizeof(xbox_KrnlVersion)==8);
        if (defaults) {
            XBOX_HARDWARE_INFO empty_hardware={0}; XBOX_KRNL_VERSION empty_version={0};
            uint8_t zero[284]={0};
            CHECK(!memcmp(&xbox_HardwareInfo,&empty_hardware,8));
            CHECK(!memcmp(&xbox_KrnlVersion,&empty_version,8));
            CHECK(!memcmp(xbox_HDKey,zero,16) && !memcmp(xbox_SignatureKey,zero,16) && !memcmp(xbox_LANKey,zero,16));
            CHECK(!memcmp(xbox_AlternateSignatureKeys,zero,256) && !memcmp(xbox_XePublicKeyData,zero,284));
        } else {
        memset(&xbox_HardwareInfo,0xA5,sizeof(xbox_HardwareInfo));
        xbox_kernel_set_version(9,8,765,4);
        for (unsigned i=0;i<16;i++) {
            xbox_HDKey[i]=(UCHAR)(i+1);
            xbox_SignatureKey[i]=(UCHAR)(i+33);
            xbox_LANKey[i]=(UCHAR)(i+65);
        }
        memset(xbox_AlternateSignatureKeys,0xC3,sizeof(xbox_AlternateSignatureKeys));
        memset(xbox_XePublicKeyData,0xD4,sizeof(xbox_XePublicKeyData));
        }
#if defined(_WIN32)
        _putenv_s("RECOMP_XBOX_HDKEY","ffffffffffffffffffffffffffffffff");
#else
        setenv("RECOMP_XBOX_HDKEY","ffffffffffffffffffffffffffffffff",1);
#endif
        *word(table_va)=0x80000142u; /* XboxHardwareInfo ordinal322 */
        xbox_kernel_init();
        CHECK(xbox_resolve_ordinal(322)==(ULONG_PTR)&xbox_HardwareInfo);
        CHECK(xbox_resolve_ordinal(324)==(ULONG_PTR)&xbox_KrnlVersion);
        CHECK(xbox_resolve_ordinal(323)==(ULONG_PTR)xbox_HDKey);
        CHECK(xbox_resolve_ordinal(325)==(ULONG_PTR)xbox_SignatureKey);
        CHECK(xbox_resolve_ordinal(353)==(ULONG_PTR)xbox_LANKey);
        CHECK(xbox_resolve_ordinal(354)==(ULONG_PTR)xbox_AlternateSignatureKeys);
        CHECK(xbox_resolve_ordinal(355)==(ULONG_PTR)xbox_XePublicKeyData);
        xbox_kernel_bridge_init();
        CHECK(*word(table_va)==XBOX_KERNEL_DATA_BASE+KDATA_HARDWARE_INFO);
        CHECK(!memcmp(word(XBOX_KERNEL_DATA_BASE+KDATA_HARDWARE_INFO),&xbox_HardwareInfo,8));
        CHECK(!memcmp(word(XBOX_KERNEL_DATA_BASE+KDATA_KRNL_VERSION),&xbox_KrnlVersion,8));
        CHECK(!memcmp(word(XBOX_KERNEL_DATA_BASE+KDATA_HD_KEY),xbox_HDKey,16));
        CHECK(!memcmp(word(XBOX_KERNEL_DATA_BASE+KDATA_SIGNATURE_KEY),xbox_SignatureKey,16));
        CHECK(!memcmp(word(XBOX_KERNEL_DATA_BASE+KDATA_LAN_KEY),xbox_LANKey,16));
        CHECK(!memcmp(word(XBOX_KERNEL_DATA_BASE+KDATA_ALT_SIGNATURE_KEYS),xbox_AlternateSignatureKeys,256));
        CHECK(!memcmp(word(XBOX_KERNEL_DATA_BASE+KDATA_XE_PUBLIC_KEY),xbox_XePublicKeyData,284));
        xbox_kernel_shutdown();
    }
    if (!strcmp(mode,"launch-page") || !strcmp(mode,"no-launch-page")) {
        XBOX_LAUNCH_DATA_PAGE page;
        int supplied = !strcmp(mode,"launch-page");
        CHECK(sizeof(page) == 4096);
        memset(&page,0xA5,sizeof(page));
        page.LaunchDataType=7; page.TitleId=0x12345678;
        xbox_LaunchDataPage = supplied ? &page : NULL;
#if defined(_WIN32)
        _putenv_s("RECOMP_CMDLINE","ignored generic runtime policy");
#else
        setenv("RECOMP_CMDLINE","ignored generic runtime policy",1);
#endif
        xbox_kernel_init();
        CHECK(*(XBOX_LAUNCH_DATA_PAGE **)xbox_resolve_ordinal(164) == xbox_LaunchDataPage);
        xbox_kernel_bridge_init();
        uint32_t guest_page = *word(XBOX_KERNEL_DATA_BASE + KDATA_LAUNCH_DATA_PAGE);
        CHECK((guest_page != 0) == supplied);
        if (supplied && guest_page) {
            CHECK(!(guest_page & 4095));
            CHECK(!memcmp(word(guest_page),&page,sizeof(page)));
            memset(&page,0,sizeof(page));
            CHECK(*word(guest_page) == 7 && *word(guest_page+4) == 0x12345678);
        }
        xbox_LaunchDataPage=NULL;
        xbox_kernel_shutdown();
    }
    if (!strcmp(mode,"sparse")) {
        /* A port may explicitly select a bounded span containing NULL gaps. */
        xbox_kernel_set_thunk_address(table_va,3);
        xbox_kernel_init();
        CHECK(xbox_kernel_thunk_table[0] && xbox_kernel_thunk_table[2]);
        xbox_kernel_bridge_init();
        CHECK(*word(table_va) == 0xFE000000 && !*word(table_va+4) &&
              *word(table_va+8) == 0xFE000008);
        CHECK(*word(table_va-4) == 0x80000067);
        xbox_kernel_shutdown();
    }
    if (!strcmp(mode,"remapped") || !strcmp(mode,"remapped-data")) {
        int data = !strcmp(mode,"remapped-data");
        unsigned short map[512] = {0};
        extern void (*recomp_lookup_kernel(uint32_t))(void);
        extern RECOMP_TLS uint32_t g_eax, g_esp;
        ULONG target = data ? 16 : 151; /* ExEventObjectType or KeStallExecutionProcessor */
        map[511] = (unsigned short)target;
        map[target] = 173; /* distinguish one translation from recursive remapping */
        xbox_kernel_set_ordinal_remap(map,512);
        CHECK(xbox_kernel_canonical_ordinal(511) == target);
        CHECK(xbox_kernel_canonical_ordinal(510) == 510 &&
              xbox_kernel_canonical_ordinal(512) == 512 &&
              xbox_kernel_canonical_ordinal(UINT32_MAX) == UINT32_MAX);
        xbox_kernel_init();
        CHECK(xbox_resolve_ordinal(target) && xbox_kernel_thunk_table[0] == xbox_resolve_ordinal(target));
        CHECK(xbox_kernel_thunk_table[1] == xbox_resolve_ordinal(data ? 151 : 173));
        xbox_kernel_bridge_init();
        if (data) {
            CHECK(*word(table_va) == XBOX_KERNEL_DATA_BASE + KDATA_EVENT_OBJ_TYPE);
        } else {
            void (*call)(void) = recomp_lookup_kernel(*word(table_va));
            uint32_t sp = XBOX_STACK_TOP;
            *word(sp) = 0; *word(sp+4) = 0; /* zero-microsecond stall */
            g_esp=sp; g_eax=0xdeadbeef;
            CHECK(call != NULL);
            if (call) { call(); CHECK(g_eax == 0 && g_esp == sp+8); }
        }
        xbox_kernel_set_ordinal_remap(map,-1);
        CHECK(xbox_kernel_canonical_ordinal(511) == 511);
        xbox_kernel_set_ordinal_remap(map,0);
        CHECK(xbox_kernel_canonical_ordinal(511) == 511);
        xbox_kernel_set_ordinal_remap(NULL,512);
        CHECK(xbox_kernel_canonical_ordinal(511) == 511);
        xbox_kernel_set_ordinal_remap(NULL,0);
        xbox_kernel_shutdown();
    }
    xbox_MemoryLayoutShutdown();
    CHECK(!XBOX_KERNEL_DATA_BASE);
    if (kdata_config) {
        CHECK(!xbox_MemoryLayoutInit(xbe,4));
        CHECK(!XBOX_KERNEL_DATA_BASE);
        CHECK(xbox_MemoryLayoutInit(xbe,0x3000));
        CHECK(XBOX_KERNEL_DATA_BASE == 0x500000u);
        xbox_MemoryLayoutShutdown();
        CHECK(xbox_ConfigureKernelData(0));
        CHECK(xbox_MemoryLayoutInit(xbe,0x3000));
        CHECK(XBOX_KERNEL_DATA_BASE == 0x31000u);
        CHECK(*word(XBOX_KERNEL_DATA_BASE) == 0);
        xbox_MemoryLayoutShutdown();
        CHECK(!XBOX_KERNEL_DATA_BASE);
    }
    xbox_kernel_get_thunk_address(&actual_base,&actual_count);
    CHECK(!actual_base && !actual_count);
    CHECK(xbox_MemoryLayoutInit(xbe,0x3000));
    if (xbox_GetMemoryBase()) {
        xbox_kernel_set_thunk_address(0,0);
        xbox_kernel_get_thunk_address(&actual_base,&actual_count);
        CHECK(!actual_base && !actual_count);
        xbox_kernel_set_thunk_address(table_va,XBOX_KERNEL_THUNK_TABLE_SIZE+1);
        xbox_kernel_get_thunk_address(&actual_base,&actual_count);
        CHECK(!actual_base && !actual_count);
        const uint32_t bad[][2] = {
            {table_va+1,1}, {0x07fffffc,2}, {0xfffffffc,2},
            {0x0000fffc,1}, {table_va,0}
        };
        for (unsigned i=0;i<sizeof bad/sizeof bad[0];++i) {
            xbox_kernel_set_thunk_address(table_va,1);
            xbox_kernel_set_thunk_address(bad[i][0],bad[i][1]);
            xbox_kernel_get_thunk_address(&actual_base,&actual_count);
            CHECK(!actual_base && !actual_count);
        }
    }
done:
    if (xbox_GetMemoryBase()) xbox_MemoryLayoutShutdown();
    free(xbe);
    if (!failures) printf("PASS thunk %s\n",mode);
    return failures ? 1 : 0;
}
