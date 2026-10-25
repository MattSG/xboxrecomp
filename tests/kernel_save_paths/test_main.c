/* Compile the actual path backend. Only the symbolic-link table and default
 * app-data location are substituted, keeping every test write in our own
 * temporary directory and avoiding any game or existing save files. */
#include "kernel.h"
#include "../kernel_test_host.h"
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <shlobj.h>
#endif

static char default_folder[MAX_PATH];
static int mounted;

#if defined(_WIN32)
/* SHGetFolderPathW is the one default-folder source the Win32 backend uses. */
static HRESULT test_default_folder(HWND window, int folder, HANDLE token,
                                   DWORD flags, LPWSTR path)
{
    (void)window; (void)folder; (void)token; (void)flags;
    return MultiByteToWideChar(CP_UTF8, 0, default_folder, -1, path, MAX_PATH)
        ? S_OK : E_FAIL;
}
#define SHGetFolderPathW test_default_folder
#endif

void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level; (void)subsystem; (void)fmt;
}

const char *xbox_LookupSymbolicLink(const char *link)
{
    if (!mounted) return NULL;
    if (mounted == 2 && strcmp(link, "\\??\\Y:") == 0)
        return "\\Device\\Custom\\";
    if (strcmp(link, "\\??\\U:") == 0)
        return "\\Device\\Harddisk0\\Partition1\\UDATA\\01234567";
    if (strcmp(link, "\\??\\T:") == 0)
        return "\\Device\\Harddisk0\\Partition1\\TDATA\\01234567";
    return NULL;
}

#include "../../src/kernel/kernel_path.c"
#if defined(_WIN32)
#undef SHGetFolderPathW
#endif

static int failures;

/* The backend returns UTF-16 on Windows and UTF-8 elsewhere; paths here are ASCII. */
static void host_narrow(const xbox_host_char *in, char *out, size_t bytes)
{
#if defined(_WIN32)
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, (int)bytes, NULL, NULL);
#else
    snprintf(out, bytes, "%s", in);
#endif
}

/* Expected host path: base, then the Xbox tail with its separators converted. */
static void expected_path(char *out, size_t bytes, const char *base, const char *tail)
{
    size_t n;
    if (!tail[0]) { snprintf(out, bytes, "%s", base); return; }
    n = (size_t)snprintf(out, bytes, "%s" XT_SEP, base);
    for (size_t i = 0; tail[i] && n + 1 < bytes; i++)
        out[n++] = tail[i] == '\\' ? XT_SEP[0] : tail[i];
    out[n] = '\0';
}

static void check_path(const char *guest, const char *base, const char *tail)
{
    xbox_host_char translated[MAX_PATH];
    char actual[MAX_PATH], expected[MAX_PATH];
    expected_path(expected, sizeof expected, base, tail);
    if (!xbox_translate_path(guest, translated, MAX_PATH)) {
        fprintf(stderr, "FAIL %s: translation failed\n", guest);
        failures++;
        return;
    }
    host_narrow(translated, actual, sizeof actual);
    if (xt_stricmp(actual, expected) != 0) {
        fprintf(stderr, "FAIL %s: got %s, expected %s\n", guest, actual, expected);
        failures++;
    }
}

static void clean_save_root(const char *base)
{
    static const char *dirs[] = { "TitleData", "UserData", "Cache",
                                  "SystemData", "UDATA", "TDATA" };
    char path[MAX_PATH], leaf[32];
    for (int i = 0; i < 6; i++) {
        snprintf(leaf, sizeof leaf, "Partition%d.img", i);
        xt_join(path, MAX_PATH, base, leaf);
        xt_delete_file(path);
        xt_join(path, MAX_PATH, base, dirs[i]);
        xt_remove_dir(path);
    }
    xt_remove_dir(base);
}

int main(void)
{
    char root[MAX_PATH], game[MAX_PATH], save[MAX_PATH];
    char path[MAX_PATH], original[MAX_PATH], isolated[MAX_PATH];
    char value[8] = {0};
    xbox_host_char translated[MAX_PATH];
    long bytes;

    if (!xt_make_temp_dir(root, sizeof root)) return 10;
    xt_join(game, MAX_PATH, root, "game");
    xt_join(save, MAX_PATH, root, "isolated");
    xt_join(default_folder, MAX_PATH, root, "default");
    xt_make_dir(game);
#if !defined(_WIN32)
    /* POSIX default save root is $XDG_DATA_HOME/xboxrecomp: point it at the
     * temporary default folder, as the Win32 mock does. */
    setenv("XDG_DATA_HOME", default_folder, 1);
#endif
    xbox_path_init(game, save);

    check_path("\\Device\\Harddisk0\\Partition1\\UDATA\\01234567\\profile", save,
               "UDATA\\01234567\\profile");
    check_path("\\device\\harddisk0\\partition1\\tdata\\01234567\\$C", save,
               "TDATA\\01234567\\$C");
    check_path("\\Device\\Harddisk0\\Partition1\\UDATA", save, "UDATA");
    check_path("\\Device\\Harddisk0\\Partition1\\TDATA\\", save, "TDATA");
    check_path("\\Device\\Harddisk0\\Partition1\\UDATA_backup\\profile", game,
               "UDATA_backup\\profile");
    check_path("\\Device\\Harddisk0\\Partition1\\TDATA2\\asset", game, "TDATA2\\asset");
    check_path("\\Device\\Harddisk0\\Partition1\\Assets\\Objects\\Example\\mesh.bin", game,
               "Assets\\Objects\\Example\\mesh.bin");
    check_path("D:\\Assets\\catalog.dat", game, "Assets\\catalog.dat");
    check_path("U:\\profile", save, "UserData\\profile");
    mounted = 1;
    check_path("U:\\ABCDEF012345\\profile", save, "UDATA\\01234567\\ABCDEF012345\\profile");
    check_path("\\??\\T:\\$u\\contentmeta.xbx", save, "TDATA\\01234567\\$u\\contentmeta.xbx");

    /* Physical system/cache defaults do not assume an extracted dashboard. */
    check_path("C:\\asset.bin", save, "SystemData\\asset.bin");
    check_path("\\??\\C:\\asset.bin", save, "SystemData\\asset.bin");
    check_path("\\??\\Y:\\asset.bin", save, "Cache\\asset.bin");
    check_path("Y:\\asset.bin", save, "Cache\\asset.bin");
    check_path("\\Device\\Harddisk0\\Partition2\\asset.bin", save,
               "SystemData\\asset.bin");
    {
        const XboxPathMount mounts[] = {
            { "C:\\", game },
            { "Y:\\", game },
            { "\\Device\\Harddisk0\\Partition2\\", game },
            { "Y:\\nested\\", save },
            { "\\Device\\Custom\\", save }
        };
        const XboxPathMount invalid[] = { { "Y:", game } };
        if (!xbox_path_set_mounts(mounts, 5)) failures++;
        check_path("c:\\asset.bin", game, "asset.bin");
        check_path("Y:\\asset.bin", game, "asset.bin");
        check_path("\\Device\\Harddisk0\\Partition2\\asset.bin", game, "asset.bin");
        check_path("Y:\\nested\\item", save, "item");
        check_path("\\Device\\Harddisk0\\Partition2\\", save, "Partition2.img");
        check_path("Y:\\nested2\\item", game, "nested2\\item");
        check_path("\\Device\\Custom\\item", save, "item");
        mounted = 2;
        check_path("Y:\\item", save, "item");
        mounted = 1;
        if (xbox_translate_path("Y:\\item", translated, 2)) failures++;
        {
            const XboxPathMount bad_root[] = { { "Y:\\", NULL } };
            const XboxPathMount bad_prefix[] = { { NULL, game } };
            if (xbox_path_set_mounts(bad_root, 1) ||
                xbox_path_set_mounts(bad_prefix, 1)) failures++;
        }
        if (xbox_path_set_mounts(invalid, 1)) failures++;
        check_path("Y:\\asset.bin", game, "asset.bin");
        if (xbox_path_set_mounts(NULL, 1) || xbox_path_set_mounts(mounts, -1)) failures++;
        {
#if defined(_WIN32)
            const char *volume_root = "C:\\";
#else
            const char *volume_root = "/";
#endif
            const XboxPathMount root_mount = { "Y:\\", volume_root };
            if (!xbox_path_set_mounts(&root_mount, 1)) failures++;
            check_path("Y:\\", volume_root, "");
            if (!xbox_path_set_mounts(NULL, 0)) failures++;
        }
        check_path("Y:\\asset.bin", save, "Cache\\asset.bin");
    }

    /* Translation does not manufacture a missing save slot or metadata. */
    xbox_translate_path("U:\\FEDCBA543210\\SaveMeta.xbx", translated, MAX_PATH);
    host_narrow(translated, path, sizeof path);
    if (xt_path_exists(path)) failures++;
    xt_join(path, MAX_PATH, save, "UDATA" XT_SEP "01234567" XT_SEP "FEDCBA543210");
    if (xt_path_exists(path)) failures++;

    /* A write using the mounted drive cannot change the legacy profile. */
    xt_join(path, MAX_PATH, game, "UDATA"); xt_make_dir(path);
    xt_join(original, MAX_PATH, game, "UDATA" XT_SEP "profile");
    if (!xt_write_file(original, "legacy", 6)) return 11;
    xt_join(path, MAX_PATH, save, "UDATA" XT_SEP "01234567"); xt_make_dir(path);
    xbox_translate_path("U:\\profile", translated, MAX_PATH);
    host_narrow(translated, isolated, sizeof isolated);
    if (!xt_write_file(isolated, "test", 4)) return 12;
    bytes = xt_read_file(original, value, sizeof value);
    if (bytes != 6 || memcmp(value, "legacy", 6) != 0) failures++;

    /* NULL and empty save roots retain the existing game_dir save layout. */
    xbox_path_init(game, NULL);
    check_path("U:\\profile", game, "UDATA\\01234567\\profile");
    check_path("T:\\$C", game, "TDATA\\01234567\\$C");
    xbox_path_init(game, "");
    check_path("\\Device\\Harddisk0\\Partition1\\UDATA\\profile", game, "UDATA\\profile");

    xt_delete_file(isolated);
    xt_join(path, MAX_PATH, save, "UDATA" XT_SEP "01234567"); xt_remove_dir(path);
    xt_delete_file(original);
    xt_join(path, MAX_PATH, game, "UDATA"); xt_remove_dir(path);
    clean_save_root(save);
    xt_join(path, MAX_PATH, default_folder, "xboxrecomp"); clean_save_root(path);
    xt_remove_dir(default_folder); xt_remove_dir(game); xt_remove_dir(root);
    if (failures) return 1;
    puts("PASS: explicit and default save routing, mounted U:/T:, asset paths, and legacy profile preservation");
    return 0;
}
