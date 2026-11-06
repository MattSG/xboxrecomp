"""Generic file helpers for the bounded game-test harness: finite-number validation,
SHA-256, link detection and a save-template copy that keeps sparse disk images sparse.

Standard library only; the sparse copy uses Windows FSCTL calls and falls back to
shutil.copy2 elsewhere.
"""
import ctypes
from ctypes import wintypes
import hashlib
import math
import os
from pathlib import Path
import shutil
import stat
import struct


def number(value, name, low=0, high=600):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or not low <= value <= high:
        raise ValueError(f"{name} must be finite and within {low}..{high}")
    return value


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def linked(path):
    return bool(path.lstat().st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT) if os.name == "nt" else path.is_symlink()


def copy_save_file(source, destination):
    """Keep Xbox partition images sparse instead of copying gigabytes of holes."""
    source, destination = Path(source), Path(destination)
    if os.name != "nt" or not source.stat().st_file_attributes & stat.FILE_ATTRIBUTE_SPARSE_FILE:
        return shutil.copy2(source, destination)
    import msvcrt
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    k.DeviceIoControl.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                 ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    k.SetFilePointerEx.argtypes = [wintypes.HANDLE, ctypes.c_longlong, ctypes.c_void_p, wintypes.DWORD]
    k.SetEndOfFile.argtypes = [wintypes.HANDLE]
    returned = wintypes.DWORD()
    size, cursor = source.stat().st_size, 0
    with source.open("rb") as src, destination.open("xb") as dst:
        source_handle, target_handle = msvcrt.get_osfhandle(src.fileno()), msvcrt.get_osfhandle(dst.fileno())
        if not k.DeviceIoControl(target_handle, 0x900C4, None, 0, None, 0, ctypes.byref(returned), None):  # FSCTL_SET_SPARSE
            raise ctypes.WinError(ctypes.get_last_error())
        # Python/CRT truncate extends by writing zeros on Windows, allocating
        # the holes. SetEndOfFile preserves sparse allocation instead.
        if not k.SetFilePointerEx(target_handle, size, None, 0) or not k.SetEndOfFile(target_handle):
            raise ctypes.WinError(ctypes.get_last_error())
        while cursor < size:
            query = ctypes.create_string_buffer(struct.pack("<qq", cursor, size - cursor))
            ranges = ctypes.create_string_buffer(65536)
            complete = k.DeviceIoControl(source_handle, 0x940CF, query, 16, ranges, len(ranges), ctypes.byref(returned), None)  # FSCTL_QUERY_ALLOCATED_RANGES
            if not complete and ctypes.get_last_error() != 234:  # ERROR_MORE_DATA
                raise ctypes.WinError(ctypes.get_last_error())
            if returned.value % 16 or (not complete and not returned.value):
                raise OSError("Invalid sparse-file extent response")
            for offset, length in struct.iter_unpack("<qq", ranges.raw[:returned.value]):
                if offset < cursor or length <= 0 or offset + length > size:
                    raise OSError("Invalid sparse-file extent")
                src.seek(offset)
                dst.seek(offset)
                remaining = length
                while remaining:
                    data = src.read(min(1024 * 1024, remaining))
                    if not data:
                        raise OSError("Save template changed during copy")
                    dst.write(data)
                    remaining -= len(data)
                cursor = offset + length
            if complete:
                break
    shutil.copystat(source, destination)
    return str(destination)


def copy_save(source, destination):
    """Never share writable caches/images or follow template junctions."""
    if linked(source):
        raise ValueError("Save template must be an ordinary directory")
    for directory, dirs, files in os.walk(source, followlinks=False):
        for name in dirs + files:
            if linked(Path(directory) / name):
                raise ValueError(f"Save template contains a link: {Path(directory) / name}")
    shutil.copytree(source, destination, copy_function=copy_save_file)
