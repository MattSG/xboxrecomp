#include "video_host_policy.h"
#include "video_player.h"
#include "kernel/kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

static void opened_video(const xbox_host_char *path)
{
#if defined(_WIN32)
    char host[MAX_PATH * 4];
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, host, sizeof host, NULL, NULL))
        return;
#else
    const char *host = path;
#endif
    size_t n = strlen(host);
    if (n > 4 && !_stricmp(host + n - 4, ".wmv") && !xbox_VideoIsPlaying())
        xbox_VideoPlayFile(host);
}

static void finish_video(ULONG routine)
{
    unsigned waited = 0;
    (void)routine;
    while (xbox_VideoIsPlaying() && waited < 60000) {
        Sleep(50);
        waited += 50;
    }
    if (waited)
        fprintf(stderr, "[VIDEO] waited %u ms before firmware return\n", waited);
}

void xbox_video_host_install_wmv_policy(void)
{
    xbox_SetFileOpenObserver(getenv("RECOMP_FMV_HOST") ? opened_video : NULL);
    xbox_SetFirmwareReturnHook(finish_video);
}
