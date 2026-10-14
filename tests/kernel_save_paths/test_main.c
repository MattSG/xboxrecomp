/* Compile the actual path backend. Only the symbolic-link table and default
 * app-data location are substituted, keeping every test write in our own
 * temporary directory and avoiding any game or existing save files. */
#include "kernel.h"
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static WCHAR default_folder[MAX_PATH];
static int mounted;

static HRESULT test_default_folder(HWND window, int folder, HANDLE token,
                                   DWORD flags, LPWSTR path)
{
    (void)window; (void)folder; (void)token; (void)flags;
    wcscpy_s(path, MAX_PATH, default_folder);
    return S_OK;
}

void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level; (void)subsystem; (void)fmt;
}

const char *xbox_LookupSymbolicLink(const char *link)
{
    if (!mounted) return NULL;
    if (strcmp(link, "\\??\\U:") == 0)
        return "\\Device\\Harddisk0\\Partition1\\UDATA\\4d53002a";
    if (strcmp(link, "\\??\\T:") == 0)
        return "\\Device\\Harddisk0\\Partition1\\TDATA\\4d53002a";
    return NULL;
}

#define SHGetFolderPathW test_default_folder
#include "../../src/kernel/kernel_path.c"
#undef SHGetFolderPathW

static int failures;

static void check_path(const char *guest, const WCHAR *base, const WCHAR *tail)
{
    WCHAR actual[MAX_PATH], expected[MAX_PATH];
    if (tail[0]) swprintf_s(expected, MAX_PATH, L"%s\\%s", base, tail);
    else wcscpy_s(expected, MAX_PATH, base);
    if (!xbox_translate_path(guest, actual, MAX_PATH) ||
        _wcsicmp(actual, expected) != 0) {
        fprintf(stderr, "FAIL %s: got %ls, expected %ls\n", guest, actual, expected);
        failures++;
    }
}

static void clean_save_root(const WCHAR *base)
{
    static const WCHAR *dirs[] = { L"TitleData", L"UserData", L"Cache",
                                    L"SystemData", L"UDATA", L"TDATA" };
    WCHAR path[MAX_PATH];
    for (int i = 0; i < 6; i++) {
        swprintf_s(path, MAX_PATH, L"%s\\Partition%d.img", base, i);
        DeleteFileW(path);
        swprintf_s(path, MAX_PATH, L"%s\\%s", base, dirs[i]);
        RemoveDirectoryW(path);
    }
    RemoveDirectoryW(base);
}

int main(void)
{
    WCHAR temp[MAX_PATH], root[MAX_PATH], game[MAX_PATH], save[MAX_PATH];
    WCHAR path[MAX_PATH], original[MAX_PATH], isolated[MAX_PATH];
    char game_utf8[MAX_PATH], save_utf8[MAX_PATH];
    DWORD bytes;
    HANDLE file;
    char value[8] = {0};
    GetTempPathW(MAX_PATH, temp);
    swprintf_s(root, MAX_PATH, L"%smm3-save-paths-%lu-%llu", temp,
        GetCurrentProcessId(), (unsigned long long)GetTickCount64());
    if (!CreateDirectoryW(root, NULL)) return 10;
    swprintf_s(game, MAX_PATH, L"%s\\game", root);
    swprintf_s(save, MAX_PATH, L"%s\\isolated", root);
    swprintf_s(default_folder, MAX_PATH, L"%s\\default", root);
    CreateDirectoryW(game, NULL);
    WideCharToMultiByte(CP_UTF8, 0, game, -1, game_utf8, MAX_PATH, NULL, NULL);
    WideCharToMultiByte(CP_UTF8, 0, save, -1, save_utf8, MAX_PATH, NULL, NULL);
    xbox_path_init(game_utf8, save_utf8);

    check_path("\\Device\\Harddisk0\\Partition1\\UDATA\\4d53002a\\profile", save,
               L"UDATA\\4d53002a\\profile");
    check_path("\\device\\harddisk0\\partition1\\tdata\\4d53002a\\$C", save,
               L"TDATA\\4d53002a\\$C");
    check_path("\\Device\\Harddisk0\\Partition1\\UDATA", save, L"UDATA");
    check_path("\\Device\\Harddisk0\\Partition1\\TDATA\\", save, L"TDATA");
    check_path("\\Device\\Harddisk0\\Partition1\\UDATA_backup\\profile", game,
               L"UDATA_backup\\profile");
    check_path("\\Device\\Harddisk0\\Partition1\\TDATA2\\asset", game, L"TDATA2\\asset");
    check_path("\\Device\\Harddisk0\\Partition1\\Data\\Shared\\Cars\\VRace\\body.cmp", game,
               L"Data\\Shared\\Cars\\VRace\\body.cmp");
    check_path("D:\\Data\\Data_hd.zip", game, L"Data\\Data_hd.zip");
    check_path("U:\\profile", save, L"UserData\\profile");
    mounted = 1;
    check_path("U:\\194916D15BE4\\profile", save, L"UDATA\\4d53002a\\194916D15BE4\\profile");
    check_path("\\??\\T:\\$u\\contentmeta.xbx", save, L"TDATA\\4d53002a\\$u\\contentmeta.xbx");

    /* Translation does not manufacture a missing save slot or metadata. */
    xbox_translate_path("U:\\96734455B22F\\SaveMeta.xbx", path, MAX_PATH);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) failures++;
    swprintf_s(path, MAX_PATH, L"%s\\UDATA\\4d53002a\\96734455B22F", save);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) failures++;

    /* A write using the mounted drive cannot change the legacy profile. */
    swprintf_s(path, MAX_PATH, L"%s\\UDATA", game); CreateDirectoryW(path, NULL);
    swprintf_s(original, MAX_PATH, L"%s\\UDATA\\profile", game);
    file = CreateFileW(original, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (file == INVALID_HANDLE_VALUE || !WriteFile(file, "legacy", 6, &bytes, NULL)) return 11;
    CloseHandle(file);
    swprintf_s(path, MAX_PATH, L"%s\\UDATA\\4d53002a", save); CreateDirectoryW(path, NULL);
    xbox_translate_path("U:\\profile", isolated, MAX_PATH);
    file = CreateFileW(isolated, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (file == INVALID_HANDLE_VALUE || !WriteFile(file, "test", 4, &bytes, NULL)) return 12;
    CloseHandle(file);
    file = CreateFileW(original, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE || !ReadFile(file, value, sizeof(value), &bytes, NULL) ||
        bytes != 6 || memcmp(value, "legacy", 6) != 0) failures++;
    CloseHandle(file);

    /* NULL and empty save roots retain the existing game_dir save layout. */
    xbox_path_init(game_utf8, NULL);
    check_path("U:\\profile", game, L"UDATA\\4d53002a\\profile");
    check_path("T:\\$C", game, L"TDATA\\4d53002a\\$C");
    xbox_path_init(game_utf8, "");
    check_path("\\Device\\Harddisk0\\Partition1\\UDATA\\profile", game, L"UDATA\\profile");

    DeleteFileW(isolated);
    swprintf_s(path, MAX_PATH, L"%s\\UDATA\\4d53002a", save); RemoveDirectoryW(path);
    DeleteFileW(original);
    swprintf_s(path, MAX_PATH, L"%s\\UDATA", game); RemoveDirectoryW(path);
    clean_save_root(save);
    swprintf_s(path, MAX_PATH, L"%s\\xboxrecomp", default_folder); clean_save_root(path);
    RemoveDirectoryW(default_folder); RemoveDirectoryW(game); RemoveDirectoryW(root);
    if (failures) return 1;
    puts("PASS: explicit and default save routing, mounted U:/T:, asset paths, and legacy profile preservation");
    return 0;
}
