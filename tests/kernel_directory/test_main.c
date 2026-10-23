#include "../../src/kernel/kernel_file.c"
/* Include the file backend to inspect its private enumeration ownership. */
#include "xbox_memory_layout.h"
#include "../kernel_test_host.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef void (*guest_fn)(void);
extern guest_fn recomp_lookup_kernel(uint32_t);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;
void *recomp_lookup(ULONG a) { (void)a; abort(); }
void *recomp_lookup_manual(ULONG a) { (void)a; abort(); }
static uint8_t *mem;
static guest_fn query, close_guest, map_status;
/* A context is searching when its native enumeration is open. Windows keeps a
 * FindFirstFile handle there; POSIX keeps the DIR stream. */
static int fail(const char *message, NTSTATUS status, ULONG_PTR information) {
    fprintf(stderr, "FAIL: %s (status=%08X information=%llu)\n", message,
            (unsigned)status, (unsigned long long)information);
    return 0;
}
static unsigned active_searches(void) {
    unsigned count = 0;
    for (unsigned i = 0; i < MAX_DIR_CONTEXTS; i++)
#if defined(_WIN32)
        if (s_dir_contexts[i].find_handle && s_dir_contexts[i].find_handle != INVALID_HANDLE_VALUE)
#else
        if (s_dir_contexts[i].dir)
#endif
            count++;
    return count;
}
static int guest_close(HANDLE h) {
    uint32_t *sp = (uint32_t *)(mem + 0x20000);
    sp[0] = 0xBEEF0001; sp[1] = (uint32_t)(uintptr_t)h;
    g_esp = 0x20000;
    close_guest=recomp_lookup_kernel(*(uint32_t *)(mem+0x10004));
    close_guest();
    return g_eax == 0 && g_esp == 0x20008;
}
static HANDLE guest_open(void) {
    uint32_t *sp=(uint32_t *)(mem+0x20000);
    memset(sp,0,64);
    *(uint32_t *)(mem+0x40000)=0;
    *(uint32_t *)(mem+0x40004)=0x40100;
    *(uint32_t *)(mem+0x40008)=0;
    memcpy(mem+0x40200,"D:\\",4);
    *(uint16_t *)(mem+0x40100)=3;
    *(uint16_t *)(mem+0x40102)=4;
    *(uint32_t *)(mem+0x40104)=0x40200;
    sp[0]=0xBEEF0001; sp[1]=0x40500; sp[2]=1; sp[3]=0x40000;
    sp[4]=0x40600; sp[5]=7; sp[6]=1;
    g_esp=0x20000;
    recomp_lookup_kernel(*(uint32_t *)(mem+0x1000C))();
    if(g_eax || g_esp!=0x2001C) return INVALID_HANDLE_VALUE;
    return (HANDLE)(uintptr_t)*(uint32_t *)(mem+0x40500);
}
static int check_status_mapping(uint32_t status, uint32_t error) {
    uint32_t *sp = (uint32_t *)(mem + 0x20000);
    sp[0] = 0xBEEF0001; sp[1] = status;
    g_esp = 0x20000;
    map_status=recomp_lookup_kernel(*(uint32_t *)(mem+0x10008));
    map_status();
    return g_eax == error && g_esp == 0x20008;
}
static int invoke(HANDLE h, const char *pattern, unsigned restart, unsigned klass) {
    uint32_t *sp=(uint32_t *)(mem+0x20000);
    memset(sp,0,64); sp[0]=0xBEEF0001;
    sp[1]=(uint32_t)(uintptr_t)h; sp[5]=0x30000; sp[6]=0x31000;
    sp[7]=512; sp[8]=klass; sp[9]=pattern?0x32000:0; sp[10]=restart;
    if(pattern) {
        *(uint16_t *)(mem+0x32000)=(uint16_t)strlen(pattern);
        *(uint16_t *)(mem+0x32002)=(uint16_t)strlen(pattern)+1;
        *(uint32_t *)(mem+0x32004)=0x32100;
        strcpy((char *)mem+0x32100,pattern);
    }
    memset(mem+0x30000,0xCC,8); memset(mem+0x31000,0xCC,512);
    g_esp=0x20000;
    query=recomp_lookup_kernel(*(uint32_t *)(mem+0x10000));
    query();
    if(g_esp!=0x2002C) { fprintf(stderr,"FAIL directory query stack: %08X expected 0002002C\n",g_esp); return 0; }
    if(*(uint32_t *)(mem+0x30000)!=g_eax) { puts("FAIL I/O status differs"); return 0; }
    return 1;
}
/* Closing the first search: a raw native handle on Windows, a guest token on POSIX. */
#if defined(_WIN32)
#define CLOSE_FIRST(h) (xbox_NtClose(h)==STATUS_SUCCESS)
#else
#define CLOSE_FIRST(h) guest_close(h)
#endif
static int short_buffer_contract(void) {
    const ULONG header = (ULONG)offsetof(XBOX_FILE_DIRECTORY_INFORMATION, FileName);
    XBOX_ANSI_STRING path = { .Buffer = "D:\\", .Length = 3, .MaximumLength = 4 };
    XBOX_ANSI_STRING pattern = { .Buffer = "Overflow9", .Length = 9, .MaximumLength = 10 };
    XBOX_OBJECT_ATTRIBUTES oa = {0};
    XBOX_IO_STATUS_BLOCK ios = {0};
    unsigned char buffer[512];
    HANDLE directory = NULL;
    oa.ObjectName = &path;
    NTSTATUS status = xbox_NtCreateFile(&directory, XBOX_GENERIC_READ, &oa, &ios,
        NULL, 0, 1, XBOX_FILE_OPEN, XBOX_FILE_DIRECTORY_FILE);
    if (status != STATUS_SUCCESS || !directory)
        return fail("open direct directory handle", status, ios.Information);
    char long_pattern[MAX_PATH + 1];
    memset(long_pattern, 'a', sizeof(long_pattern));
    pattern.Buffer = long_pattern;
    pattern.Length = MAX_PATH;
    pattern.MaximumLength = sizeof(long_pattern);
    memset(buffer, 0xA5, sizeof(buffer));
    status = xbox_NtQueryDirectoryFile(directory, NULL, NULL, NULL, &ios,
        buffer, sizeof(buffer), XboxFileDirectoryInformation, &pattern, TRUE);
    if (status != STATUS_INVALID_PARAMETER || ios.Status != status || ios.Information)
        return fail("overlong counted ANSI pattern status/info", status, ios.Information);
    for (size_t i = 0; i < sizeof(buffer); i++)
        if (buffer[i] != 0xA5)
            return fail("overlong pattern modified output", status, ios.Information);
    pattern.Buffer = "Overflow9";
    pattern.Length = 9;
    pattern.MaximumLength = 10;
    memset(buffer, 0xA5, sizeof(buffer));
    const ULONG minimum = sizeof(XBOX_FILE_DIRECTORY_INFORMATION);
    status = xbox_NtQueryDirectoryFile(directory, NULL, NULL, NULL, &ios,
        buffer, minimum - 1, XboxFileDirectoryInformation, &pattern, TRUE);
    if (status != STATUS_INFO_LENGTH_MISMATCH || ios.Status != status || ios.Information)
        return fail("short directory structure status/info", status, ios.Information);
    for (size_t i = 0; i < sizeof(buffer); i++)
        if (buffer[i] != 0xA5)
            return fail("short directory header modified buffer", status, ios.Information);
    status = xbox_NtQueryDirectoryFile(directory, NULL, NULL, NULL, &ios,
        buffer, minimum, XboxFileDirectoryInformation, &pattern, TRUE);
    XBOX_FILE_DIRECTORY_INFORMATION *entry = (void *)buffer;
    if (status != STATUS_BUFFER_OVERFLOW || ios.Status != status ||
        ios.Information != minimum || entry->FileNameLength != 9 ||
        memcmp(entry->FileName, "Overflow", 8))
        return fail("fixed header overflow status/info", status, ios.Information);
    memset(buffer, 0xA5, sizeof(buffer));
    status = xbox_NtQueryDirectoryFile(directory, NULL, NULL, NULL, &ios,
        buffer, minimum, XboxFileDirectoryInformation, &pattern, FALSE);
    if (status != STATUS_SUCCESS || ios.Status != status || ios.Information)
        return fail("subsequent short entry reports success with no structure", status, ios.Information);
    for (size_t i = 0; i < sizeof(buffer); i++)
        if (buffer[i] != 0xA5)
            return fail("subsequent short entry modified output", status, ios.Information);
    memset(buffer, 0xA5, sizeof(buffer));
    status = xbox_NtQueryDirectoryFile(directory, NULL, NULL, NULL, &ios,
        buffer, header + 9, XboxFileDirectoryInformation, &pattern, FALSE);
    entry = (void *)buffer;
    if (status != STATUS_SUCCESS || ios.Status != status || ios.Information != header + 9 ||
        entry->FileNameLength != 9 || memcmp(entry->FileName, "Overflow9", 9))
        return fail("overflow retry did not return pending entry", status, ios.Information);
    for (size_t i = header + 9; i < sizeof(buffer); i++)
        if (buffer[i] != 0xA5)
            return fail("query modified bytes past returned record", status, ios.Information);
    status = xbox_NtQueryDirectoryFile(directory, NULL, NULL, NULL, &ios,
        buffer, header + 9, XboxFileDirectoryInformation, &pattern, FALSE);
    if (status != STATUS_NO_MORE_FILES || ios.Status != status || ios.Information)
        return fail("repeated EOF status/info", status, ios.Information);
    status = xbox_NtQueryDirectoryFile(directory, NULL, NULL, NULL, &ios,
        buffer, header + 9, XboxFileDirectoryInformation, &pattern, FALSE);
    if (status != STATUS_NO_MORE_FILES || ios.Status != status || ios.Information)
        return fail("repeated EOF stays exhausted until restart", status, ios.Information);
    pattern.Buffer = "Save*";
    pattern.Length = 5;
    pattern.MaximumLength = 6;
    status = xbox_NtQueryDirectoryFile(directory, NULL, NULL, NULL, &ios,
        buffer, header + 9, XboxFileDirectoryInformation, &pattern, TRUE);
    entry = (void *)buffer;
    if (status != STATUS_SUCCESS || ios.Status != status || entry->FileNameLength != 9 ||
        memcmp(entry->FileName, "Overflow9", 9))
        return fail("restart after EOF returns first match", status, ios.Information);
    xbox_NtClose(directory);
    return 1;
}
int main(void) {
    /* This fixture owns fixed raw scratch addresses below the export page. */
    if (!xbox_ConfigureKernelData(0x00740000u)) return 90;

    char path[MAX_PATH], child[MAX_PATH];
    if(!xt_make_temp_dir(path,sizeof path)) return 10;
    xt_join(child,sizeof child,path,"SaveSlot"); xt_make_dir(child);
    char overflow_child[MAX_PATH];
    xt_join(overflow_child,sizeof overflow_child,path,"Overflow9"); xt_make_dir(overflow_child);
#if !defined(_WIN32)
    /* The default save root and its partition images go in a sibling directory,
     * so the searched directory holds only the entries this test creates. */
    char save_root[MAX_PATH];
    if(!xt_make_temp_dir(save_root,sizeof save_root)) return 10;
    setenv("XDG_DATA_HOME", save_root, 1);
#endif
    mem=VirtualAlloc(NULL,16*1024*1024,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); if(!mem) return 12;
    g_xbox_mem_offset=(ptrdiff_t)mem;
    *(uint32_t *)(mem+0x10000)=0x800000CF;
    *(uint32_t *)(mem+0x10004)=0x800000BB;
    *(uint32_t *)(mem+0x10008)=0x8000012D;
    *(uint32_t *)(mem+0x1000C)=0x800000CA;
    xbox_path_init(path,NULL);
    xbox_kernel_set_thunk_address(0x10000,4); xbox_kernel_bridge_init();
    query=recomp_lookup_kernel(*(uint32_t *)(mem+0x10000));
    close_guest=recomp_lookup_kernel(*(uint32_t *)(mem+0x10004));
    map_status=recomp_lookup_kernel(*(uint32_t *)(mem+0x10008));
    /* The guest holds a bridge token, never a raw host HANDLE: a raw value is
     * refused by bridge_resolve_handle. D:\ maps to the game directory, which is path. */
    HANDLE h=guest_open();
    if(h==INVALID_HANDLE_VALUE) return 11;
    int ok=short_buffer_contract() && invoke(h,"Save*",1,1);
    XBOX_FILE_DIRECTORY_INFORMATION *e=(void *)(mem+0x31000);
    if(ok && (g_eax || e->FileNameLength!=8 || memcmp(e->FileName,"SaveSlot",8) || !(e->FileAttributes&16))) {
        fprintf(stderr,"FAIL filtered directory: status=%08X length=%lu\n",g_eax,e->FileNameLength); ok=0;
    }
    if(ok) ok=invoke(h,NULL,0,1) && g_eax==0x80000006u;
    if(ok) ok=invoke(h,"Save*",1,1) && g_eax==0;
    if(ok) ok=invoke(h,"*",1,1) && g_eax==0 && e->FileNameLength==8 && !memcmp(e->FileName,"SaveSlot",8);
    if(ok) ok=invoke(h,"*",1,2) && g_eax==0xC0000003u;
    if(ok) ok=check_status_mapping(0x80000006u,18) && check_status_mapping(0,0)
        && check_status_mapping(0xC0000008u,6);
    if(ok) ok=active_searches()==1 && guest_close(h) && active_searches()==0;
    else guest_close(h);
    /* Guest NtClose consumes a tagged table token, not a raw Windows handle.
     * Open through ordinal 202 so this exercises the real bridge contract. */
    for(unsigned round=0;ok && round<MAX_DIR_CONTEXTS+8;round++) {
        h=guest_open();
        if(h==INVALID_HANDLE_VALUE) {ok=0;break;}
        ok=invoke(h,"Save*",0,1) && g_eax==0 && active_searches()==1;
        if(!ok) fprintf(stderr,"FAIL guest open round %u: status=%08X searches=%u\n",round,g_eax,active_searches());
        ok=guest_close(h) && ok && active_searches()==0;
        if(!ok) fprintf(stderr,"FAIL guest close round %u: searches=%u\n",round,active_searches());
    }
    h=guest_open();
    if(ok) ok=invoke(h,"Save*",1,1) && g_eax==0
        && invoke(h,NULL,0,1) && g_eax==0x80000006u;
    if(h!=INVALID_HANDLE_VALUE) guest_close(h);
    h=guest_open();
    if(ok) ok=active_searches()==0 && invoke(h,"Save*",0,1) && g_eax==0;
    if(h!=INVALID_HANDLE_VALUE) guest_close(h);
#if defined(_WIN32)
    xt_remove_dir(overflow_child); xt_remove_dir(child); xt_remove_dir(path);
#else
    xt_remove_tree(path);
    xt_remove_tree(save_root);
#endif
    VirtualFree(mem,0,MEM_RELEASE);
    if(!ok) return 1;
    puts("PASS: directory ABI/filter/restart/status, guest/direct close, 144 slot-reuse cycles and fresh searches"); return 0;
}
