/* Real settings implementation and native save-root translation, no game/GPU. */
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <process.h>
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

void xbox_log(int level, const char *subsystem, const char *format, ...)
{ (void)level; (void)subsystem; (void)format; }
const char *xbox_LookupSymbolicLink(const char *name) { (void)name; return NULL; }

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

static intptr_t child(const char *exe, const char *mode, const char *root)
{
#if defined(_WIN32)
    return _spawnl(_P_NOWAIT, exe, exe, mode, root, NULL);
#else
    pid_t pid = fork();
    if (!pid) { execl(exe, exe, mode, root, (char *)NULL); _exit(127); }
    return pid;
#endif
}

static int completed(intptr_t pid)
{
    int status;
    if (pid < 0) return 0;
#if defined(_WIN32)
    return _cwait(&status, pid, 0) != -1 && status == 0;
#else
    return waitpid((pid_t)pid, &status, 0) == pid &&
           WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
}

static int query(ULONG index, ULONG expected)
{
    ULONG value = 0xdeadbeef, type = 0, length = 0;
    return xbox_ExQueryNonVolatileSetting(index, &type, &value, 4, &length) == 0 &&
           value == expected && type == 4 && length == 4;
}

static int clean_root(const char *root)
{
    char path[MAX_PATH + 64];
    const char *directories[] = {
        "TitleData", "UserData", "Cache", "SystemData", "UDATA", "TDATA"
    };
#if !defined(_WIN32)
#define _rmdir rmdir
#endif
    snprintf(path, sizeof(path), "%s/NonVolatileSettings.bin", root);
    remove(path);
    _rmdir(path); /* the write-failure case deliberately made this a directory */
    snprintf(path, sizeof(path), "%s/NonVolatileSettings.bin.lock", root);
    remove(path);
    for (int i = 0; i < 6; i++) {
        snprintf(path, sizeof(path), "%s/Partition%d.img", root, i);
        remove(path);
        snprintf(path, sizeof(path), "%s/%s", root, directories[i]);
        _rmdir(path);
    }
    return _rmdir(root) == 0;
}

int main(int argc, char **argv)
{
    char root[MAX_PATH], other[MAX_PATH + 32], file[MAX_PATH + 32];
    ULONG value, type, length;
    unsigned char block[100];
    FILE *stream;
    intptr_t first, second;
    if (argc == 3) {
        xbox_path_init(".", argv[2]);
        if (!strcmp(argv[1], "read"))
            return query(XC_LANGUAGE, 9) ? 0 : 1;
        if (!strcmp(argv[1], "reader")) {
            for (unsigned i = 0; i < 5000; i++)
                CHECK(xbox_ExQueryNonVolatileSetting(XC_MAX_OS, &type, block,
                                                    sizeof(block), &length) == 0);
            return 0;
        }
        for (value = 0; value < 20; value++)
            CHECK(xbox_ExSaveNonVolatileSetting(
                !strcmp(argv[1], "language") ? XC_LANGUAGE : XC_AUDIO,
                4, &value, 4) == 0);
        return 0;
    }
#if defined(_WIN32)
    CHECK(GetTempPathA(sizeof(root), root));
    snprintf(other, sizeof(other), "%sxbox-settings-%lu", root, GetCurrentProcessId());
    strcpy(root, other);
    CHECK(CreateDirectoryA(root, NULL));
    _putenv_s("RECOMP_VIDEO_FLAGS", "");
#else
    strcpy(root, "/tmp/xbox-settings-XXXXXX");
    CHECK(mkdtemp(root));
    unsetenv("RECOMP_VIDEO_FLAGS");
#endif
    xbox_path_init(".", root);
    CHECK(query(XC_LANGUAGE, 1));
    CHECK(query(XC_AUDIO, 0));
    CHECK(query(XC_DVD_REGION, 1));
    value = 0xdeadbeef; type = 0xfeed; length = 0;
    CHECK((uint32_t)xbox_ExQueryNonVolatileSetting(XC_LANGUAGE, &type,
          &value, 3, &length) == 0xc0000023u);
    CHECK(length == 4 && value == 0xdeadbeef && type == 0xfeed);
    CHECK(xbox_ExQueryNonVolatileSetting(XC_LANGUAGE, NULL, NULL, 4, NULL) ==
          STATUS_INVALID_PARAMETER);
    CHECK(xbox_ExQueryNonVolatileSetting(0xfe, NULL, &value, 4, NULL) ==
          STATUS_OBJECT_NAME_NOT_FOUND);
    value = 9;
    /* The original kernel ignores Type; it must not be mistaken for a
     * request to change a field's intrinsic REG_DWORD / REG_BINARY type. */
    CHECK(xbox_ExSaveNonVolatileSetting(XC_LANGUAGE, 3, &value, 4) == 0);
    CHECK(query(XC_LANGUAGE, 9));
    CHECK(completed(child(argv[0], "read", root)));
    CHECK(xbox_ExSaveNonVolatileSetting(XC_LANGUAGE, 4, &value, 5) ==
          STATUS_INVALID_PARAMETER);
    CHECK(xbox_ExSaveNonVolatileSetting(0x101, 3, &value, 4) ==
          STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(xbox_ExSaveNonVolatileSetting(XC_AUDIO, 4, NULL, 1) ==
          STATUS_INVALID_PARAMETER);
    CHECK(xbox_ExSaveNonVolatileSetting(XC_MISC, 4, &value, 1) == 0);
    CHECK(query(XC_MISC, 9)); /* short writes zero-fill the rest of the field */
    memset(block, 0xaa, sizeof(block));
    CHECK(xbox_ExQueryNonVolatileSetting(XC_MAX_OS, &type, block,
                                        sizeof(block), &length) == 0);
    CHECK(type == 3 && length == 96 && block[96] == 0 && block[99] == 0);
    memcpy(&value, block + 48, 4);
    CHECK(value == 9);
    value = 7;
    memcpy(block + 48, &value, 4);
    CHECK(xbox_ExSaveNonVolatileSetting(XC_MAX_OS, 4, block, 96) == 0);
    CHECK(query(XC_LANGUAGE, 7)); /* aggregate and scalar settings share storage */
    value = 8;
    CHECK(xbox_ExSaveNonVolatileSetting(XC_VIDEO, 4, &value, 4) == 0);
#if defined(_WIN32)
    CHECK(_putenv_s("RECOMP_VIDEO_FLAGS", "10") == 0);
#else
    CHECK(setenv("RECOMP_VIDEO_FLAGS", "10", 1) == 0);
#endif
    CHECK(query(XC_VIDEO, 0x10));
#if defined(_WIN32)
    CHECK(_putenv_s("RECOMP_VIDEO_FLAGS", "") == 0);
#else
    CHECK(unsetenv("RECOMP_VIDEO_FLAGS") == 0);
#endif
    CHECK(query(XC_VIDEO, 8)); /* host override must not overwrite saved video flags */
    CHECK(xbox_ExQueryNonVolatileSetting(XC_TZ_STD_NAME, &type, block, 4, NULL) == 0);
    CHECK(type == 3);

    snprintf(other, sizeof(other), "%s/independent", root);
    xbox_path_init(".", other);
    CHECK(query(XC_LANGUAGE, 1));
    value = 2;
    CHECK(xbox_ExSaveNonVolatileSetting(XC_LANGUAGE, 4, &value, 4) == 0);
    xbox_path_init(".", root);
    CHECK(query(XC_LANGUAGE, 7));
    first = child(argv[0], "language", root);
    second = child(argv[0], "audio", root);
    CHECK(completed(first) && completed(second));
    CHECK(query(XC_LANGUAGE, 19) && query(XC_AUDIO, 19));
    /* Real queries overlap real atomic replacements in distinct processes.
     * Readers must hold the image's shared lock until their handle closes,
     * otherwise Windows can reject saves with ERROR_SHARING_VIOLATION. */
    first = child(argv[0], "reader", root);
    second = child(argv[0], "language", root);
    CHECK(completed(first) && completed(second));
    CHECK(query(XC_LANGUAGE, 19));
    snprintf(file, sizeof(file), "%s/NonVolatileSettings.bin", root);
    stream = fopen(file, "wb");
    CHECK(stream && fwrite("bad", 1, 3, stream) == 3);
    CHECK(fclose(stream) == 0);
    CHECK((uint32_t)xbox_ExQueryNonVolatileSetting(XC_LANGUAGE, NULL,
                                                &value, 4, NULL) == 0xc000009cu);
    CHECK((uint32_t)xbox_ExSaveNonVolatileSetting(XC_LANGUAGE, 4,
                                               &value, 4) == 0xc000009cu);
    memset(block, 0, 96); /* full-size image with an invalid checksum */
    stream = fopen(file, "wb");
    CHECK(stream && fwrite(block, 1, 96, stream) == 96);
    CHECK(fclose(stream) == 0);
    CHECK((uint32_t)xbox_ExQueryNonVolatileSetting(XC_LANGUAGE, NULL,
                                                &value, 4, NULL) == 0xc000009cu);
    CHECK(remove(file) == 0);
#if defined(_WIN32)
    CHECK(_mkdir(file) == 0);
#else
    CHECK(mkdir(file, 0700) == 0);
#endif
    CHECK(xbox_ExSaveNonVolatileSetting(XC_LANGUAGE, 4, &value, 4) != 0);
    CHECK(clean_root(other));
    CHECK(clean_root(root));
    puts("PASS: persistent settings, ABI errors, corruption, isolated roots, concurrent writers");
    return 0;
}
