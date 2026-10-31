"""Process CPU time, creation-time copy and hidden launch helpers for benchmark runners.

Windows uses kernel32 through ctypes; POSIX reads /proc. Shared by any recomp
benchmark harness, so it lives with the bench tooling in the runtime.
"""
import ctypes
import os
import subprocess


def _kernel32():
    from ctypes import wintypes

    class FILETIME(ctypes.Structure):
        _fields_ = [("lo", wintypes.DWORD), ("hi", wintypes.DWORD)]

    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    k32.OpenProcess.restype = wintypes.HANDLE
    k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k32.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(FILETIME)] * 4
    k32.CreateFileW.restype = wintypes.HANDLE
    k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    k32.SetFileTime.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(FILETIME)] * 3
    k32.CloseHandle.argtypes = [wintypes.HANDLE]
    return k32, FILETIME


def process_cpu_ms(pid):
    """Total user+kernel CPU time of a process, in milliseconds."""
    if os.name != "nt":
        # /proc/<pid>/stat: after the ')' of the command name, field 3 is index 0,
        # so utime (field 14) and stime (field 15) are indexes 11 and 12.
        with open(f"/proc/{pid}/stat") as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) * 1000.0 / os.sysconf("SC_CLK_TCK")
    k32, FILETIME = _kernel32()
    handle = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        created, exited, kernel, user = FILETIME(), FILETIME(), FILETIME(), FILETIME()
        if not k32.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited),
                                   ctypes.byref(kernel), ctypes.byref(user)):
            raise ctypes.WinError(ctypes.get_last_error())
        ticks = lambda f: (f.hi << 32) | f.lo  # noqa: E731 - 100 ns units
        return (ticks(kernel) + ticks(user)) / 10000.0
    finally:
        k32.CloseHandle(handle)


def copy_creation_time(src, dst):
    """Give dst the creation time of src (the game keys its archive cache on it)."""
    if os.name != "nt":
        print("warning: POSIX cannot set a file's creation time; the game will rebuild "
              "its archive cache for this run", flush=True)
        return
    k32, FILETIME = _kernel32()
    ticks = int(os.path.getctime(src) * 10_000_000) + 116_444_736_000_000_000
    stamp = FILETIME(ticks & 0xFFFFFFFF, ticks >> 32)
    handle = k32.CreateFileW(dst, 0x100, 0x7, None, 3, 0x80, None)  # FILE_WRITE_ATTRIBUTES, OPEN_EXISTING
    if not handle or handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        if not k32.SetFileTime(handle, ctypes.byref(stamp), None, None):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        k32.CloseHandle(handle)


def hidden_startupinfo():
    if os.name != "nt":
        return None
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = subprocess.SW_HIDE
    return si
