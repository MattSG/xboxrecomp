#include "kernel.h"
#include "xbox_memory_layout.h"
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
int main(void) {
    char root[MAX_PATH], path[MAX_PATH], save[MAX_PATH];
    GetTempPathA(MAX_PATH,root); sprintf(path,"%sxml1-dir-test-%lu",root,GetCurrentProcessId());
    if(!CreateDirectoryA(path,NULL)) return 10;
    char child[MAX_PATH]; sprintf(child,"%s\\SaveSlot",path); CreateDirectoryA(child,NULL);
    sprintf(save,"%s\\SaveData",path); if(!CreateDirectoryA(save,NULL)) return 13;
    HANDLE h=CreateFileA(path,FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    if(h==INVALID_HANDLE_VALUE||(uintptr_t)h>UINT32_MAX) return 11;
    char xbe_path[MAX_PATH], image[MAX_PATH]; sprintf(xbe_path,"%s\\default.xbe",path);
    uint8_t xbe[0x18C]={0}; uint32_t base=0x10000, cert=0x10180, title_id=0x12345678;
    memcpy(xbe,"XBEH",4); memcpy(xbe+0x104,&base,4); memcpy(xbe+0x118,&cert,4); memcpy(xbe+0x188,&title_id,4);
    HANDLE xbe_file=CreateFileA(xbe_path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    DWORD written; if(xbe_file==INVALID_HANDLE_VALUE||!WriteFile(xbe_file,xbe,sizeof(xbe),&written,NULL)||written!=sizeof(xbe)) return 14;
    CloseHandle(xbe_file);
    sprintf(image,"%s\\Partition0.img",save);
    HANDLE image_file=CreateFileA(image,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    LARGE_INTEGER offset; offset.QuadPart=0x800;
    const char legacy_header[]="****PARTINFO****";
    if(image_file==INVALID_HANDLE_VALUE||!SetFilePointerEx(image_file,offset,NULL,FILE_BEGIN)||!WriteFile(image_file,legacy_header,16,&written,NULL)||written!=16) return 15;
    CloseHandle(image_file);
    mem=VirtualAlloc(NULL,16*1024*1024,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE); if(!mem) return 12;
    g_xbox_mem_offset=(ptrdiff_t)mem;
    *(uint32_t *)(mem+0x10000)=0x800000CF;
    *(uint32_t *)(mem+0x10004)=0x800000CA;
    *(uint32_t *)(mem+0x10008)=0x800000BB;
    *(uint32_t *)(mem+0x1000C)=0x800000E2;
    xbox_kernel_set_thunk_address(0x10000,4); xbox_kernel_bridge_init();
    int ok=1;
    /* XFindFirstFile shortens ANSI_STRING.Length to exclude its wildcard,
     * without inserting a NUL at the new end of the directory name. */
    xbox_path_init(path,save);
    uint8_t sector[512];
    image_file=CreateFileA(image,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    offset.QuadPart=0x800;
    if(image_file==INVALID_HANDLE_VALUE||!SetFilePointerEx(image_file,offset,NULL,FILE_BEGIN)||!ReadFile(image_file,sector,sizeof(sector),&written,NULL)||written!=sizeof(sector)) {
        fprintf(stderr,"FAIL reading seeded Partition0 table\n"); ok=0;
    } else {
        uint32_t magic,version,actual_title_id,cache_partition,active,sentinel;
        memcpy(&magic,sector,4); memcpy(&version,sector+4,4); memcpy(&actual_title_id,sector+8,4);
        memcpy(&cache_partition,sector+12,4); memcpy(&active,sector+16,4); memcpy(&sentinel,sector+0x1F8,4);
        if(magic!=0x97315286u||version!=2||actual_title_id!=title_id||cache_partition!=0||active!=1||sentinel!=0xAA550000u) {
            fprintf(stderr,"FAIL Partition0 title table: %08X %u %08X %u %u %08X\n",magic,version,actual_title_id,cache_partition,active,sentinel); ok=0;
        }
        uint32_t marker=0xCAFEBABEu; offset.QuadPart=0x820;
        if(!SetFilePointerEx(image_file,offset,NULL,FILE_BEGIN)||!WriteFile(image_file,&marker,sizeof(marker),&written,NULL)||written!=sizeof(marker)) ok=0;
    }
    if(image_file!=INVALID_HANDLE_VALUE) CloseHandle(image_file);
    xbox_path_init(path,save);
    image_file=CreateFileA(image,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    uint32_t persisted=0; offset.QuadPart=0x820;
    if(image_file==INVALID_HANDLE_VALUE||!SetFilePointerEx(image_file,offset,NULL,FILE_BEGIN)||!ReadFile(image_file,&persisted,sizeof(persisted),&written,NULL)||written!=sizeof(persisted)||persisted!=0xCAFEBABEu) {
        fprintf(stderr,"FAIL preserving valid Partition0 table\n"); ok=0;
    }
    if(image_file!=INVALID_HANDLE_VALUE) CloseHandle(image_file);
    *(uint32_t *)(mem+0x40000)=0;
    *(uint32_t *)(mem+0x40004)=0x40100;
    *(uint32_t *)(mem+0x40008)=0;
    const char counted[]="D:\\SaveSlot*ignored";
    memcpy(mem+0x40200,counted,sizeof(counted));
    *(uint16_t *)(mem+0x40100)=11;
    *(uint16_t *)(mem+0x40102)=sizeof(counted);
    *(uint32_t *)(mem+0x40104)=0x40200;
    uint32_t *sp=(uint32_t *)(mem+0x20000);memset(sp,0,64);
    sp[0]=0xBEEF0001;sp[1]=0x40500;sp[2]=1;sp[3]=0x40000;
    sp[4]=0x40600;sp[5]=7;sp[6]=1;
    g_esp=0x20000;recomp_lookup_kernel(*(uint32_t *)(mem+0x10004))();
    if(g_eax || g_esp!=0x2001C) {
        fprintf(stderr,"FAIL counted directory name: status=%08X ESP=%08X\n",g_eax,g_esp);ok=0;
    }
    if(!g_eax) {
        uint32_t token=*(uint32_t *)(mem+0x40500);
        char saved[MAX_PATH]; sprintf(saved,"%s\\save.dat",child);
        HANDLE file=CreateFileA(saved,GENERIC_WRITE,7,NULL,CREATE_ALWAYS,0,NULL);
        DWORD written; WriteFile(file,"roundtrip",9,&written,NULL); CloseHandle(file);
        *(uint32_t *)(mem+0x40000)=token;
        memcpy(mem+0x40200,"save.dat",9);
        *(uint16_t *)(mem+0x40100)=8; *(uint16_t *)(mem+0x40102)=9;
        memset(sp,0,64);sp[0]=0xBEEF0001;sp[1]=0x40504;sp[2]=0x00110100;
        sp[3]=0x40000;sp[4]=0x40600;sp[5]=7;sp[6]=0x4040;
        g_esp=0x20000;recomp_lookup_kernel(*(uint32_t *)(mem+0x10004))();
        if(g_eax || g_esp!=0x2001C) {
            fprintf(stderr,"FAIL relative save open: status=%08X ESP=%08X\n",g_eax,g_esp);ok=0;
        } else {
            uint32_t file_token=*(uint32_t *)(mem+0x40504);
            mem[0x40700]=1;memset(sp,0,64);
            sp[0]=0xBEEF0001;sp[1]=file_token;sp[2]=0x40600;
            sp[3]=0x40700;sp[4]=1;sp[5]=13;
            g_esp=0x20000;recomp_lookup_kernel(*(uint32_t *)(mem+0x1000C))();
            if(g_eax || g_esp!=0x20018) ok=0;
            sp[0]=0xBEEF0001;sp[1]=file_token;g_esp=0x20000;
            recomp_lookup_kernel(*(uint32_t *)(mem+0x10008))();
            if(g_eax || GetFileAttributesA(saved)!=INVALID_FILE_ATTRIBUTES) {
                puts("FAIL relative save delete-on-close");ok=0;
            }
        }
        DeleteFileA(saved);
        sp[0]=0xBEEF0001;sp[1]=token;g_esp=0x20000;
        recomp_lookup_kernel(*(uint32_t *)(mem+0x10008))();
        if(g_eax || g_esp!=0x20008) ok=0;
    }
    CloseHandle(h); RemoveDirectoryA(child); DeleteFileA(xbe_path);
    for(int p=0;p<=5;p++) { sprintf(image,"%s\\Partition%d.img",save,p); DeleteFileA(image); }
    const char *save_dirs[]={"TitleData","UserData","Cache","SystemData"};
    for(int i=0;i<4;i++) { sprintf(image,"%s\\%s",save,save_dirs[i]); RemoveDirectoryA(image); }
    RemoveDirectoryA(save); RemoveDirectoryA(path); VirtualFree(mem,0,MEM_RELEASE);
    if(!ok) return 1;
    puts("PASS: counted object names, relative save opens, deletion, and Xbox stack cleanup"); return 0;
}
