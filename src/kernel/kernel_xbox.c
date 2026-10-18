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
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#if defined(_WIN32)
#include <io.h>
#endif
#include "platform/win32_compat.h"
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

/* ============================================================================
 * Exported Data Objects
 *
 * These are global variables exported by the Xbox kernel at known ordinals.
 * Game code accesses them directly via the thunk table.
 * ============================================================================ */

/* Hardware info - default report is a standard retail Xbox.
 * A target may override via xbox_kernel_set_version() or by patching the
 * global directly; no title-specific values are baked in. */
XBOX_HARDWARE_INFO xbox_HardwareInfo = {
    .Flags       = 0x00000020,  /* Retail Xbox */
    .GpuRevision = 0xD2,       /* NV2A D2 revision */
    .McpRevision = 0xD4,       /* MCPX D4 revision */
    .Reserved    = {0, 0}
};

/* Kernel version - default is a stock retail value. Titles built against a
 * different XDK can report their own through xbox_kernel_set_version(). */
XBOX_KRNL_VERSION xbox_KrnlVersion = {
    .Major = 1,
    .Minor = 0,
    .Build = 5849,
    .Qfe   = 1
};

/* Override the kernel version reported to game code (ordinal 324).
 * Match the XDK revision the target title was built with if it checks it. */
void xbox_kernel_set_version(USHORT major, USHORT minor, USHORT build, USHORT qfe)
{
    xbox_KrnlVersion.Major = major;
    xbox_KrnlVersion.Minor = minor;
    xbox_KrnlVersion.Build = build;
    xbox_KrnlVersion.Qfe   = qfe;
}

/* Crypto keys - zeroed, not needed for PC operation.
 * These are unique per-console on real hardware. */
UCHAR xbox_EEPROMKey[16]              = {0};
UCHAR xbox_HDKey[16]                  = {0};
UCHAR xbox_SignatureKey[16]           = {0};
UCHAR xbox_LANKey[16]                 = {0};
UCHAR xbox_AlternateSignatureKeys[16][16] = {{0}};

/* Public key data for Xbox Live signature verification - not needed */
UCHAR xbox_XePublicKeyData[284] = {0};

/* Image filename - the XBE path as seen by the kernel */
static char g_image_filename[] = "\\Device\\CdRom0\\default.xbe";
XBOX_ANSI_STRING xbox_XeImageFileName = {
    .Length        = sizeof(g_image_filename) - 1,
    .MaximumLength = sizeof(g_image_filename),
    .Buffer        = g_image_filename
};

/* Launch data page - used for title-to-title launches (e.g., Xbox Dashboard → game).
 * Allocated dynamically and zeroed for normal game boot. */
static XBOX_LAUNCH_DATA_PAGE g_launch_data_page = {0};
XBOX_LAUNCH_DATA_PAGE* xbox_LaunchDataPage = &g_launch_data_page;

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
 * Non-volatile OS settings
 *
 * Persist the 96-byte Xbox user-settings block, including its checksum, in
 * the configured save root. This is an HLE settings store, not a complete
 * hardware EEPROM image (the console's encrypted/factory areas are separate).
 * Field offsets and checksum semantics: https://xboxdevwiki.net/EEPROM
 * ============================================================================ */
#define SETTINGS_DATA_ERROR ((NTSTATUS)0xC000009CL)
#define SETTINGS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#define SETTINGS_SIZE 96
static SRWLOCK s_settings_lock = SRWLOCK_INIT;

static int settings_field(ULONG index, ULONG *offset, ULONG *length, ULONG *type)
{
    static const unsigned char offsets[] = {
        4, 8, 12, 24, 28, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92
    };
    if (index <= XC_DVD_REGION) {
        *offset = offsets[index];
        *length = 4;
        *type = (index == XC_TZ_STD_NAME || index == XC_TZ_DLT_NAME) ? 3 : 4;
    } else if (index == XC_MAX_OS) {
        *offset = 0;
        *length = SETTINGS_SIZE;
        *type = 3;
    } else if (index == 0x101) {
        *offset = 0;
        *length = 6;
        *type = 3;
    } else {
        return 0;
    }
    return 1;
}

/* End-around-carry checksum: a valid block, checksum included, sums to -1. */
static uint32_t settings_sum(const unsigned char *data)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < SETTINGS_SIZE; i += 4) {
        uint32_t word;
        memcpy(&word, data + i, 4);
        sum += word;
    }
    while (sum >> 32) sum = (uint32_t)sum + (sum >> 32);
    return (uint32_t)sum;
}

static void settings_checksum(unsigned char *data)
{
    uint32_t checksum;
    memset(data, 0, 4);
    checksum = ~settings_sum(data);
    memcpy(data, &checksum, 4);
}

static void settings_defaults(unsigned char *data)
{
    ULONG word;
    memset(data, 0, SETTINGS_SIZE);
    word = (ULONG)-300; memcpy(data + 4, &word, 4);
    word = 1; memcpy(data + 48, &word, 4); /* English */
    word = XC_VIDEO_FLAGS_WIDESCREEN | XC_VIDEO_FLAGS_HDTV;
    memcpy(data + 52, &word, 4);
    word = 1; memcpy(data + 92, &word, 4); /* DVD region */
    settings_checksum(data);
}

static int settings_path(xbox_host_char *path, const char *suffix)
{
    xbox_host_char directory[MAX_PATH];
    if (!xbox_get_save_directory(directory, MAX_PATH))
        return 0;
#if defined(_WIN32)
    return swprintf_s(path, MAX_PATH, L"%ls\\NonVolatileSettings.bin%hs",
                      directory, suffix) > 0;
#else
    int n = snprintf(path, MAX_PATH, "%s/NonVolatileSettings.bin%s", directory, suffix);
    return n > 0 && n < MAX_PATH;
#endif
}

static FILE *settings_open(const xbox_host_char *path, int write)
{
#if defined(_WIN32)
    return _wfopen(path, write ? L"wb" : L"rb");
#else
    return fopen(path, write ? "wb" : "rb");
#endif
}

static NTSTATUS settings_load(const xbox_host_char *path, unsigned char *data)
{
    FILE *file = settings_open(path, 0);
    int extra, failed;
    size_t length;
    if (!file) {
        if (errno != ENOENT) return STATUS_UNSUCCESSFUL;
        settings_defaults(data);
        return STATUS_SUCCESS;
    }
    length = fread(data, 1, SETTINGS_SIZE, file);
    extra = fgetc(file);
    failed = ferror(file);
    if (fclose(file)) failed = 1;
    if (failed) return STATUS_UNSUCCESSFUL;
    if (length != SETTINGS_SIZE || extra != EOF || settings_sum(data) != UINT32_MAX)
        return SETTINGS_DATA_ERROR;
    return STATUS_SUCCESS;
}

static void settings_mac(unsigned char *mac)
{
    const char *env = getenv("RECOMP_NET_MAC");
    unsigned v[6];
    if (env && sscanf(env, "%x:%x:%x:%x:%x:%x",
                      &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
        for (int i = 0; i < 6; i++) mac[i] = (uint8_t)v[i];
    } else {
        char name[64] = "xbox";
        DWORD n = sizeof name;
        const char *port = getenv("RECOMP_NET_PORT");
        uint32_t h = 2166136261u;
        GetComputerNameA(name, &n);
        for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        for (const char *p = port ? port : ""; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
        mac[0] = 0; mac[1] = 0x50; mac[2] = 0xF2;
        mac[3] = (uint8_t)(h >> 16); mac[4] = (uint8_t)(h >> 8); mac[5] = (uint8_t)h;
    }
}

#if defined(_WIN32)
typedef HANDLE SettingsLock;
#define SETTINGS_LOCK_FAILED INVALID_HANDLE_VALUE
#else
typedef int SettingsLock;
#define SETTINGS_LOCK_FAILED (-1)
#endif

static void settings_unlock_file(SettingsLock lock)
{
#if defined(_WIN32)
    CloseHandle(lock);
#else
    close(lock);
#endif
}

/* A separate lock inode survives replacement of the settings image. Shared
 * readers close their image handle before releasing it: on Windows even an
 * ordinary CRT reader can otherwise prevent the writer's atomic replacement. */
static SettingsLock settings_lock_file(const xbox_host_char *path, int write)
{
    SettingsLock lock;
#if defined(_WIN32)
    OVERLAPPED overlapped = {0};
    lock = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (lock == SETTINGS_LOCK_FAILED) return lock;
    if (LockFileEx(lock, write ? LOCKFILE_EXCLUSIVE_LOCK : 0,
                   0, 1, 0, &overlapped)) return lock;
#else
    lock = open(path, O_RDWR | O_CREAT, 0600);
    if (lock == SETTINGS_LOCK_FAILED) return lock;
    if (!flock(lock, write ? LOCK_EX : LOCK_SH)) return lock;
#endif
    settings_unlock_file(lock);
    return SETTINGS_LOCK_FAILED;
}

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
    unsigned char data[SETTINGS_SIZE];
    xbox_host_char path[MAX_PATH], lockpath[MAX_PATH];
    SettingsLock lock;
    ULONG offset, length, type;
    NTSTATUS status;
    if (!settings_field(ValueIndex, &offset, &length, &type))
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (ResultLength) *ResultLength = length;
    if (ValueLength < length) return SETTINGS_BUFFER_TOO_SMALL;
    if (!Value) return STATUS_INVALID_PARAMETER;
    if (ValueIndex == 0x101) {
        settings_mac(data);
    } else {
        if (!settings_path(path, "") || !settings_path(lockpath, ".lock"))
            return STATUS_UNSUCCESSFUL;
        AcquireSRWLockExclusive(&s_settings_lock);
        lock = settings_lock_file(lockpath, 0);
        status = lock == SETTINGS_LOCK_FAILED ? STATUS_UNSUCCESSFUL
                                              : settings_load(path, data);
        if (lock != SETTINGS_LOCK_FAILED) settings_unlock_file(lock);
        ReleaseSRWLockExclusive(&s_settings_lock);
        if (status != STATUS_SUCCESS) return status;
        /* Explicit host video selection overrides dashboard settings. */
        if (ValueIndex == XC_VIDEO || ValueIndex == XC_MAX_OS) {
            const char *env = getenv("RECOMP_VIDEO_FLAGS");
            if (env) {
                ULONG video = (ULONG)strtoul(env, NULL, 16);
                memcpy(data + 52, &video, 4);
                settings_checksum(data);
            }
        }
    }
    if (Type) *Type = type;
    memset(Value, 0, ValueLength);
    memcpy(Value, data + offset, length);
    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_ExSaveNonVolatileSetting(
    ULONG ValueIndex, ULONG Type, PVOID Value, ULONG ValueLength)
{
    unsigned char data[SETTINGS_SIZE];
    xbox_host_char path[MAX_PATH], temporary[MAX_PATH], lockpath[MAX_PATH];
    ULONG offset, length, type;
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    FILE *file;
    SettingsLock lock;
    /* The console ignores Type. Retail kernels do not permit factory writes. */
    (void)Type;
    if (!settings_field(ValueIndex, &offset, &length, &type) || ValueIndex > XC_MAX_OS)
        return STATUS_OBJECT_NAME_NOT_FOUND;
    if (ValueLength > length || (!Value && ValueLength))
        return STATUS_INVALID_PARAMETER;
    if (!settings_path(path, "") || !settings_path(temporary, ".tmp") ||
        !settings_path(lockpath, ".lock")) return STATUS_UNSUCCESSFUL;
    AcquireSRWLockExclusive(&s_settings_lock);
    lock = settings_lock_file(lockpath, 1);
    if (lock == SETTINGS_LOCK_FAILED) goto unlock_thread;
    status = settings_load(path, data);
    if (status != STATUS_SUCCESS) goto unlock_file;
    memset(data + offset, 0, length);
    if (ValueLength) memcpy(data + offset, Value, ValueLength);
    settings_checksum(data);
    status = STATUS_UNSUCCESSFUL;
    file = settings_open(temporary, 1);
    if (!file) goto unlock_file;
    {
        int failed = fwrite(data, 1, SETTINGS_SIZE, file) != SETTINGS_SIZE;
        if (fflush(file)) failed = 1;
#if defined(_WIN32)
        if (_commit(_fileno(file))) failed = 1;
#else
        if (fsync(fileno(file))) failed = 1;
#endif
        if (fclose(file)) failed = 1;
        if (failed) goto remove_temporary;
    }
#if defined(_WIN32)
    if (MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
#else
    if (!rename(temporary, path))
#endif
        status = STATUS_SUCCESS;
remove_temporary:
#if defined(_WIN32)
    if (status != STATUS_SUCCESS) _wremove(temporary);
#else
    if (status != STATUS_SUCCESS) remove(temporary);
#endif
unlock_file:
    settings_unlock_file(lock);
unlock_thread:
    ReleaseSRWLockExclusive(&s_settings_lock);
    if (status != STATUS_SUCCESS)
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_XBOX,
                 "ExSaveNonVolatileSetting: index=0x%02X status=0x%08X",
                 ValueIndex, (unsigned)status);
    return status;
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
