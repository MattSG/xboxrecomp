/* cl /nologo /W4 tests/pvideo_scanout.c /Fe:<scratch>/pvideo_check.exe */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/video/pvideo_scanout.h"

int main(void)
{
    uint32_t regs[0x1000 / 4] = {0}, before[0x1000 / 4];
    uint32_t dst[8] = {0};
    const uint8_t yuy2[] = {16,128,235,128,81,90,81,240};
    regs[NV_PVIDEO_BUFFER/4] = 1;
    regs[NV_PVIDEO_LIMIT/4] = sizeof yuy2;
    regs[NV_PVIDEO_SIZE_IN/4] = (2u<<16)|2u;
    regs[NV_PVIDEO_SIZE_OUT/4] = (2u<<16)|2u;
    regs[NV_PVIDEO_FORMAT/4] = 0x10004;
    regs[NV_PVIDEO_DS_DX/4] = regs[NV_PVIDEO_DT_DY/4] = 0x100000;
    memcpy(before, regs, sizeof regs);
    assert(pvideo_scanout(regs,yuy2,sizeof yuy2,dst,4,2,0));
    assert(dst[0]==0xff000000 && dst[1]==0xffffffff);
    assert(dst[4]==0xffff0000 && dst[5]==0xffff0000);
    assert(!memcmp(before,regs,sizeof regs));
    regs[NV_PVIDEO_STOP/4]=1;
    memset(dst,0,sizeof dst);
    assert(!pvideo_scanout(regs,yuy2,sizeof yuy2,dst,4,2,0) && !dst[0]);
    regs[NV_PVIDEO_STOP/4]=0;
    assert(!pvideo_scanout(regs,yuy2,7,dst,4,2,0));
    regs[NV_PVIDEO_LIMIT/4]=7;
    assert(!pvideo_scanout(regs,yuy2,8,dst,4,2,0));
    regs[NV_PVIDEO_LIMIT/4]=8;
    regs[NV_PVIDEO_POINT_IN/4]=16;
    regs[NV_PVIDEO_POINT_OUT/4]=1;
    regs[NV_PVIDEO_DS_DX/4]=0x80000;
    regs[NV_PVIDEO_FORMAT/4]|=NV_PVIDEO_FORMAT_DISPLAY;
    dst[2]=0xff112233;
    assert(pvideo_scanout(regs,yuy2,8,dst,4,2,0));
    assert(dst[1]==0xffffffff && dst[2]==0xff112233 && dst[0]==0);
    regs[NV_PVIDEO_BUFFER/4]=0;
    assert(!pvideo_scanout(regs,yuy2,8,dst,4,2,0));
    puts("PVIDEO YUY2, crop, scale, key, stop and bounds checks passed");
    return 0;
}
