#include "kernel.h"
#include "xbox_memory_layout.h"
#include <stdio.h>
void *recomp_lookup(ULONG address) { (void)address; return NULL; }
void *recomp_lookup_manual(ULONG address) { (void)address; return NULL; }
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s\n", #x); return 1; } } while (0)
int main(void)
{
    XBOX_MM_STATISTICS stats;
    CHECK(xbox_MmQueryStatistics(NULL) == STATUS_INVALID_PARAMETER);
    xbox_SetTotalRam(XBOX_DEVKIT_RAM);
    CHECK(xbox_MmQueryStatistics(&stats) == STATUS_SUCCESS);
    CHECK(stats.TotalPhysicalPages == XBOX_DEVKIT_RAM / 4096);
    CHECK(stats.AvailablePages <= stats.TotalPhysicalPages);
    xbox_SetTotalRam(XBOX_TOTAL_RAM);
    CHECK(xbox_MmQueryStatistics(&stats) == STATUS_SUCCESS);
    CHECK(stats.TotalPhysicalPages == XBOX_TOTAL_RAM / 4096);
    CHECK(stats.Length == sizeof stats);
    return 0;
}
