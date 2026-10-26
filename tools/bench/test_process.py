import importlib.util
import io
import os
from pathlib import Path
import contextlib
import subprocess
import tempfile
import time
import unittest

SPEC = importlib.util.spec_from_file_location('bench_process', Path(__file__).resolve().parent / 'process.py')
PROCESS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PROCESS)


class ProcessHelperTests(unittest.TestCase):
    def test_cpu_time_grows_with_work(self):
        before = PROCESS.process_cpu_ms(os.getpid())
        end = time.perf_counter() + 0.05
        while time.perf_counter() < end:
            pass
        after = PROCESS.process_cpu_ms(os.getpid())
        self.assertGreaterEqual(before, 0)
        self.assertGreater(after, before)

    @unittest.skipUnless(os.name == 'nt', 'creation times are Windows-only')
    def test_copy_creation_time_matches_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'source.bin'
            target = Path(temporary) / 'target.bin'
            source.write_bytes(b'a')
            target.write_bytes(b'b')
            os.utime(source, (1_000_000, 1_000_000))
            PROCESS.copy_creation_time(str(source), str(target))
            self.assertAlmostEqual(os.path.getctime(target), os.path.getctime(source), delta=0.01)

    @unittest.skipIf(os.name == 'nt', 'POSIX warning path')
    def test_posix_creation_time_warns_and_keeps_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / 'target.bin'
            target.write_bytes(b'b')
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                PROCESS.copy_creation_time(__file__, str(target))
            self.assertIn('cannot set a file', out.getvalue())
            self.assertEqual(target.read_bytes(), b'b')

    def test_hidden_startupinfo(self):
        info = PROCESS.hidden_startupinfo()
        if os.name == 'nt':
            self.assertEqual(info.wShowWindow, subprocess.SW_HIDE)
        else:
            self.assertIsNone(info)


if __name__ == '__main__':
    unittest.main()
