import io
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import process_support as ps


class LockTest(unittest.TestCase):
    def test_second_holder_is_refused_until_release(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "lock")
            first = ps.acquire_lock(path)
            self.assertIsNotNone(first)
            self.assertIsNone(ps.acquire_lock(path))
            os.close(first)
            second = ps.acquire_lock(path)
            self.assertIsNotNone(second)
            os.close(second)


class RunLogTest(unittest.TestCase):
    def test_lines_are_tagged_and_flushed_in_order(self):
        with tempfile.TemporaryDirectory() as folder, mock.patch("sys.stdout", new=io.StringIO()):
            path = os.path.join(folder, "run.log")
            log = ps.RunLog(path)
            log.write("out", b"hello\r\n")
            log.write("err", b"bad \xff bytes\n")
            log.handle.close()
            lines = open(path, encoding="utf-8").read().splitlines()
            self.assertTrue(lines[0].endswith("[out] hello"))
            self.assertTrue(lines[1].endswith("[err] bad \ufffd bytes"))


class RunningPidsTest(unittest.TestCase):
    def test_unknown_process_has_no_pids(self):
        self.assertEqual(ps.running_pids("no-such-process-for-contract-xyz"), [])


if __name__ == "__main__":
    unittest.main()
