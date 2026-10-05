#ifndef RECOMP_PROFILE_H
#define RECOMP_PROFILE_H
#ifdef RECOMP_TRACY
#include <tracy/TracyC.h>
#define RECOMP_PROFILE_BEGIN(name) TracyCZoneN(recomp_profile_zone, name, 1)
#define RECOMP_PROFILE_END() TracyCZoneEnd(recomp_profile_zone)
#define RECOMP_PROFILE_FRAME() TracyCFrameMark
#define RECOMP_PROFILE_PLOT(name, value) TracyCPlot(name, value)
#else
#define RECOMP_PROFILE_BEGIN(name) ((void)0)
#define RECOMP_PROFILE_END() ((void)0)
#define RECOMP_PROFILE_FRAME() ((void)0)
#define RECOMP_PROFILE_PLOT(name, value) ((void)0)
#endif
#endif
