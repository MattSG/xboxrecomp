#ifndef RECOMP_FRAME_LIMIT_H
#define RECOMP_FRAME_LIMIT_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

/* RECOMP_FPS_LIMIT: 0 is unpaced, a positive integer is the rate. An invalid
 * value falls back to 60, but the first one is named in the log so a mistyped
 * setting is visible. An empty or unset value is simply the default. */
static inline int recomp_frame_limit_rejected(const char *value)
{
    static int reported;
    if (!reported) {
        reported = 1;
        fprintf(stderr, "[FRAME_LIMIT] ignoring invalid frame limit \"%s\" (want 0 for unpaced or a positive integer); using 60\n",
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
    return recomp_frame_limit_parse(getenv("RECOMP_FPS_LIMIT"));
}

#endif
