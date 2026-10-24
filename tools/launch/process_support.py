"""Launcher plumbing for a recompiled game: process lookup, exclusive file
locks, and one ordered log for both child pipes.

Nothing here knows a title's name. The caller supplies the process name, the
lock path and the log path."""
import csv
import datetime as dt
import os
import subprocess
import sys
import threading

IS_WINDOWS = os.name == "nt"
if IS_WINDOWS:
    import msvcrt
else:
    import fcntl


def windows_image_name(process_name):
    """The image name tasklist reports for an executable, lower case."""
    name = process_name.lower()
    return name if name.endswith(".exe") else name + ".exe"


def running_pids(process_name):
    """Sorted pids of processes whose executable is process_name."""
    if IS_WINDOWS:
        # Localized "INFO:" text can be in a code page the locale decoder rejects.
        # Windows image names carry the extension, so accept the bare name too.
        wanted = windows_image_name(process_name)
        result = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True,
                                errors="replace")
        rows = csv.reader(result.stdout.splitlines())
        pids = [row[1] for row in rows if len(row) > 1 and row[0].lower() == wanted]
    else:
        try:
            result = subprocess.run(["pgrep", "-x", process_name], capture_output=True, text=True)
            pids = result.stdout.split()
        except FileNotFoundError:
            pids = []
            for entry in os.listdir("/proc") if os.path.isdir("/proc") else []:
                if not entry.isdigit():
                    continue
                try:
                    with open(f"/proc/{entry}/comm", encoding="utf-8") as handle:
                        if handle.read().strip() == process_name:
                            pids.append(entry)
                except OSError:
                    continue
    return sorted(int(pid) for pid in pids)


def try_lock(fd):
    try:
        if IS_WINDOWS:
            os.lseek(fd, 0, os.SEEK_SET)
            msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError:
        return False


def acquire_lock(path):
    """Return a locked fd for path, or None when another holder has it.
    Closing the fd releases the lock."""
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
    if try_lock(fd):
        return fd
    os.close(fd)
    return None


def echo(text):
    try:
        sys.stdout.write(text + "\n")
        sys.stdout.flush()
    except (OSError, ValueError):
        pass


class RunLog:
    """Serialises lines from both pipes into one log, flushing each line."""

    def __init__(self, path):
        self.handle = open(path, "x", encoding="utf-8", newline="\n")
        self.lock = threading.Lock()
        self.capture_error = None

    def write(self, source, raw):
        text = raw.decode("utf-8", "replace").rstrip("\r\n")
        stamp = dt.datetime.now(dt.timezone.utc).isoformat(timespec="microseconds")
        with self.lock:
            try:
                self.handle.write(f"{stamp} [{source}] {text}\n")
                self.handle.flush()
            except (OSError, ValueError) as error:
                if self.capture_error is None:
                    self.capture_error = repr(error)
            echo(text)


def pump(stream, source, log):
    try:
        for raw in iter(stream.readline, b""):
            log.write(source, raw)
    finally:
        stream.close()
