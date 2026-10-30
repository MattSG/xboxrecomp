#ifndef RECOMP_FRAME_LIMIT_H
#define RECOMP_FRAME_LIMIT_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* A refresh rate worth pacing to: the 0 and 1 Hz "default adapter" answers and
 * anything beyond real panels fall back to 60. */
static inline int recomp_frame_limit_from_refresh(int hz)
{
    return hz >= 30 && hz <= 500 ? hz : 60;
}

static inline int recomp_display_refresh(void)
{
#ifdef _WIN32
    /* user32 is bound at run time so callers need no extra import library. */
    typedef BOOL (WINAPI *EnumSettings)(LPCWSTR, DWORD, DEVMODEW *);
    HMODULE user32 = LoadLibraryW(L"user32.dll");
    EnumSettings enum_settings = user32 ? (EnumSettings)(void *)GetProcAddress(user32, "EnumDisplaySettingsW") : NULL;
    DEVMODEW mode;
    memset(&mode, 0, sizeof mode);
    mode.dmSize = sizeof mode;
    if (enum_settings && enum_settings(NULL, ENUM_CURRENT_SETTINGS, &mode))
        return recomp_frame_limit_from_refresh((int)mode.dmDisplayFrequency);
#endif
    return 60;
}

/* RECOMP_FPS_LIMIT: 0 is unpaced, a positive integer is the rate. An invalid
 * value falls back to 60, but the first one is named in the log so a mistyped
 * setting is visible. "auto", or an unset RECOMP_FPS_LIMIT, follows the
 * monitor's refresh rate (60 where it cannot be read). */
static inline int recomp_frame_limit_rejected(const char *value)
{
    static int reported;
    if (!reported) {
        reported = 1;
        fprintf(stderr, "[FRAME_LIMIT] ignoring invalid frame limit \"%s\" (want 0 for unpaced, auto or a positive integer); using 60\n",
                value);
    }
    return 60;
}

static inline int recomp_frame_limit_parse(const char *value)
{
    char *end;
    long requested;
    if (!value || !*value)
        return 60;
    if (!strcmp(value, "auto"))
        return recomp_display_refresh();
    errno = 0;
    requested = strtol(value, &end, 10);
    if (end == value)
        return recomp_frame_limit_rejected(value);
    while (isspace((unsigned char)*end))
        ++end;
    if (*end || errno == ERANGE || requested < 0 || requested > INT_MAX)
        return recomp_frame_limit_rejected(value);
    return (int)requested;
}

static inline int recomp_frame_limit_from_env(void)
{
    const char *value = getenv("RECOMP_FPS_LIMIT");
    return value && *value ? recomp_frame_limit_parse(value) : recomp_display_refresh();
}

#endif
