# Explicit save-directory routing

Windows regression that compiles the real `kernel_path.c` backend and uses
temporary directories. XAPI's mounted U:/T: drives resolve to Partition1 UDATA
and TDATA paths. These paths must use a supplied save root rather than bypassing
it and writing to the extracted game directory. With no supplied root the
existing game-directory layout remains available.

The test covers mounted and direct paths, case-insensitive components, similarly
named asset directories, missing metadata, and an isolated write that preserves
a legacy profile. It substitutes only the symbolic-link table and default
app-data directory; it never opens game assets or user saves.

```powershell
cmake -S tools/xboxrecomp/tests/kernel_save_paths -B build/save-paths
cmake --build build/save-paths --config Release
ctest --test-dir build/save-paths -C Release --output-on-failure
```
