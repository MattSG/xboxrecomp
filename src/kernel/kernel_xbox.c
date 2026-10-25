/*
 * kernel_xbox.c - Xbox Identity & Hardware Stubs
 *
 * Provides Xbox-specific exported data (hardware info, kernel version,
 * keys, image filename) and section loading stubs.
 *
 * Most Xbox-specific hardware data is stubbed with plausible retail
 * values. Crypto keys are zeroed since we don't need Xbox Live or
 * EEPROM-based encryption on PC.
 */

#include "kernel.h"
#include <string.h>
#include <time.h>

/* ============================================================================
 * Exported Data Objects
 *
 * These are global variables exported by the Xbox kernel at known ordinals.
 * Game code accesses them directly via the thunk table.
 * ============================================================================ */

/* Unconfigured identity exports are zero. Ports choose any compatibility
 * values explicitly before native/bridge initialization. These values do not
 * certify hardware emulation or an SDK-specific kernel ABI. */
XBOX_HARDWARE_INFO xbox_HardwareInfo = {0};
XBOX_KRNL_VERSION xbox_KrnlVersion = {0};

/* Set caller-selected reported version; bridge initialization copies it. */
void xbox_kernel_set_version(USHORT major, USHORT minor, USHORT build, USHORT qfe)
{
    xbox_KrnlVersion.Major = major;
    xbox_KrnlVersion.Minor = minor;
    xbox_KrnlVersion.Build = build;
    xbox_KrnlVersion.Qfe   = qfe;
}

/* Console credentials; zero until supplied by the port. */
UCHAR xbox_EEPROMKey[16]              = {0};
UCHAR xbox_HDKey[16]                  = {0};
UCHAR xbox_SignatureKey[16]           = {0};
UCHAR xbox_LANKey[16]                 = {0};
UCHAR xbox_AlternateSignatureKeys[16][16] = {{0}};

/* Public-key bytes; zero until supplied by the port. */
UCHAR xbox_XePublicKeyData[284] = {0};

/* Caller-owned counted image path; zero means unconfigured. */
XBOX_ANSI_STRING xbox_XeImageFileName = {0};

/* Optional port-owned boot input. Native consumers use this host pointer;
 * bridge initialization copies its complete 4096-byte guest layout. */
XBOX_LAUNCH_DATA_PAGE* xbox_LaunchDataPage = NULL;

/* ============================================================================
 * Section Loading
 *
 * XeLoadSection/XeUnloadSection manage on-demand loading of XBE sections.
 * On Xbox, some sections are demand-paged from disc. In our recompilation,
 * the entire executable is loaded into memory, so these are reference-counting
 * no-ops.
 * ============================================================================ */

NTSTATUS __stdcall xbox_XeLoadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    InterlockedIncrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeLoadSection: '%s' at %p (size=%u, refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        Section->VirtualAddress, Section->VirtualSize,
        Section->SectionReferenceCount);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_XeUnloadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    LONG new_count = InterlockedDecrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeUnloadSection: '%s' (refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        new_count);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * EEPROM / Non-Volatile Settings
 *
 * ExQueryNonVolatileSetting / ExSaveNonVolatileSetting read and write EEPROM
 * settings. On real hardware these are stored in the 256-byte EEPROM on the
 * SMBus. For recompilation, we return sensible defaults:
 *   - Region: North America
 *   - Video: NTSC, widescreen+HDTV enabled
 *   - Language: English
 *   - Audio: Stereo, Dolby Digital
 *   - DVD Region: Region 1 (North America)
 *   - Time zone: the host's
 * ============================================================================ */

/* The console's time zone. The EEPROM holds the bias (UTC = local + bias,
 * in minutes), the standard and daylight biases added to it, four-character
 * names, and the dates daylight time starts and ends (month, week of the
 * month, day of the week, hour). XAPI turns the UTC clock into local time
 * with these, so a fixed zone stamps saves and clocks hours off anywhere
 * else: SSX 3 dated its saves two hours early in CEST.
 *
 * Windows has all of it (GetTimeZoneInformation). POSIX hosts give the
 * offset in effect now and the zone names, not the rule: the bias is the
 * current offset with no daylight dates, which is right until the next
 * change of clocks during a run. */
typedef struct { uint8_t month, day, dow, hour; } XboxTzDate;
static struct {
    int32_t bias, std_bias, dlt_bias;
    XboxTzDate std_date, dlt_date;
    char std_name[4], dlt_name[4];
} s_tz;

static void tz_name4(char out[4], const char *src)
{
    int k;
    for (k = 0; k < 4; k++) {
        char c = (src && *src) ? *src++ : 0;
        out[k] = (c >= 0x20 && c < 0x7F) ? c : (c ? '?' : 0);
    }
}

static void tz_init(void)
{
    static int done;
    if (done)
        return;
    done = 1;
#ifdef _WIN32
    {
        TIME_ZONE_INFORMATION tzi;
        char sn[5], dn[5];
        int k;
        memset(&tzi, 0, sizeof tzi);
        GetTimeZoneInformation(&tzi);
        s_tz.bias = tzi.Bias;
        s_tz.std_bias = tzi.StandardBias;
        s_tz.dlt_bias = tzi.DaylightBias;
        s_tz.std_date.month = (uint8_t)tzi.StandardDate.wMonth;
        s_tz.std_date.day = (uint8_t)tzi.StandardDate.wDay;
        s_tz.std_date.dow = (uint8_t)tzi.StandardDate.wDayOfWeek;
        s_tz.std_date.hour = (uint8_t)tzi.StandardDate.wHour;
        s_tz.dlt_date.month = (uint8_t)tzi.DaylightDate.wMonth;
        s_tz.dlt_date.day = (uint8_t)tzi.DaylightDate.wDay;
        s_tz.dlt_date.dow = (uint8_t)tzi.DaylightDate.wDayOfWeek;
        s_tz.dlt_date.hour = (uint8_t)tzi.DaylightDate.wHour;
        for (k = 0; k < 4; k++) {
            sn[k] = (char)(tzi.StandardName[k] < 128 ? tzi.StandardName[k] : '?');
            dn[k] = (char)(tzi.DaylightName[k] < 128 ? tzi.DaylightName[k] : '?');
        }
        sn[4] = dn[4] = 0;
        tz_name4(s_tz.std_name, sn);
        tz_name4(s_tz.dlt_name, dn);
    }
#else
    {
        time_t now = time(NULL);
        struct tm lt;
        tzset();
        if (localtime_r(&now, &lt))
            s_tz.bias = (int32_t)(-lt.tm_gmtoff / 60);
        tz_name4(s_tz.std_name, tzname[0]);
        tz_name4(s_tz.dlt_name, tzname[1]);
    }
#endif
}

NTSTATUS __stdcall xbox_ExQueryNonVolatileSetting(
    ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength, PULONG ResultLength)
{
    if (!Value)
        return STATUS_INVALID_PARAMETER;
    tz_init();

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "ExQueryNonVolatileSetting: index=0x%02X len=%u", ValueIndex, ValueLength);

    switch (ValueIndex) {
    case XC_LANGUAGE:
        /* English = 1 */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 1;
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_VIDEO:
        /* NTSC with widescreen and HDTV support enabled */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = XC_VIDEO_FLAGS_WIDESCREEN | XC_VIDEO_FLAGS_HDTV;
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_AUDIO:
        /* Stereo PCM matches the two-channel host output. 1 means mono;
         * advertising AC3 requires an encoded-output path. */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0x00000000;
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_P_CONTROL_GAMES:
    case XC_P_CONTROL_MOVIES:
        /* Unrestricted. Titles compare their XBE certificate's rating against
         * this and refuse to run (Halo boots to the dashboard) if the console
         * is configured more strictly, so 0 means "no restriction". */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_DVD_REGION:
        /* Region 1 (North America) */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 1;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_MISC:
        /* Misc flags: 0 = no special flags */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_TIMEZONE_BIAS:
    case XC_TZ_STD_BIAS:
    case XC_TZ_DLT_BIAS:
        if (ValueLength >= sizeof(LONG)) {
            *(PLONG)Value = ValueIndex == XC_TIMEZONE_BIAS ? s_tz.bias
                          : ValueIndex == XC_TZ_STD_BIAS ? s_tz.std_bias : s_tz.dlt_bias;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(LONG);
        }
        break;

    case XC_TZ_STD_NAME:
    case XC_TZ_DLT_NAME:
        if (ValueLength >= 4) {
            memcpy(Value, ValueIndex == XC_TZ_STD_NAME ? s_tz.std_name : s_tz.dlt_name, 4);
            if (Type) *Type = 3;                 /* REG_BINARY */
            if (ResultLength) *ResultLength = 4;
        }
        break;

    case XC_TZ_STD_DATE:
    case XC_TZ_DLT_DATE:
        if (ValueLength >= 4) {
            memcpy(Value, ValueIndex == XC_TZ_STD_DATE ? &s_tz.std_date : &s_tz.dlt_date, 4);
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = 4;
        }
        break;

    case XC_MAX_OS: {
        /* The whole user-settings block (EEPROM 0x60..), which XAPI reads in
         * one go: checksum, bias, std/dlt names, 8 reserved, std/dlt dates,
         * 8 reserved, std/dlt bias, language, video, audio, parental x3,
         * online x4, misc, DVD region -- the same values as the single
         * queries above. Answering it with zeroes put every title in UTC. */
        uint8_t blk[0x60];
        memset(blk, 0, sizeof blk);
        memcpy(blk + 0x04, &s_tz.bias, 4);
        memcpy(blk + 0x08, s_tz.std_name, 4);
        memcpy(blk + 0x0C, s_tz.dlt_name, 4);
        memcpy(blk + 0x18, &s_tz.std_date, 4);
        memcpy(blk + 0x1C, &s_tz.dlt_date, 4);
        memcpy(blk + 0x28, &s_tz.std_bias, 4);
        memcpy(blk + 0x2C, &s_tz.dlt_bias, 4);
        { ULONG v = 1; memcpy(blk + 0x30, &v, 4); }                       /* English */
        { ULONG v = XC_VIDEO_FLAGS_WIDESCREEN | XC_VIDEO_FLAGS_HDTV; memcpy(blk + 0x34, &v, 4); }
        /* 0x58 is MiscFlags (0); DvdRegion is the last field, at 0x5C. */
        { ULONG v = 1; memcpy(blk + 0x5C, &v, 4); }                       /* DVD region 1 */
        if (Type) *Type = 3;
        if (ResultLength) *ResultLength = sizeof blk;
        if (ValueLength < sizeof blk)
            return 0xC0000023L;                       /* STATUS_BUFFER_TOO_SMALL */
        memcpy(Value, blk, sizeof blk);
        if (ValueLength > sizeof blk)
            memset((uint8_t *)Value + sizeof blk, 0, ValueLength - sizeof blk);
        break;
    }

    default:
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_XBOX,
            "ExQueryNonVolatileSetting: unhandled index 0x%02X", ValueIndex);
        memset(Value, 0, ValueLength);
        if (Type) *Type = 4;
        if (ResultLength) *ResultLength = ValueLength;
        break;
    }

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_ExSaveNonVolatileSetting(
    ULONG ValueIndex, ULONG Type, PVOID Value, ULONG ValueLength)
{
    (void)Type;
    (void)Value;
    (void)ValueLength;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_XBOX,
        "ExSaveNonVolatileSetting: index=0x%02X len=%u (ignored - read-only on PC)",
        ValueIndex, ValueLength);

    /* Accept writes silently but don't persist them */
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Network / PHY
 *
 * PhyGetLinkState reports Ethernet link status. Xbox had a built-in 100Mbit
 * NIC. For PC, we report link-up since we'll handle networking differently.
 * ============================================================================ */

ULONG __stdcall xbox_PhyGetLinkState(BOOLEAN Verify)
{
    (void)Verify;
    /* Link up, 100 Mbps, full duplex: XNET_ETHERNET_LINK_ACTIVE |
     * _100MBPS | _FULL_DUPLEX, the bits the kernel reads off the PHY. */
    return 0x01 | 0x02 | 0x08;
}

NTSTATUS __stdcall xbox_PhyInitialize(BOOLEAN ForceReset, PVOID Param2)
{
    (void)ForceReset;
    (void)Param2;
    return STATUS_SUCCESS;
}
