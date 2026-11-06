import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('harness_util', Path(__file__).resolve().parent / 'harness_util.py')
UTIL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(UTIL)


class HarnessUtilTests(unittest.TestCase):
    def test_number_accepts_finite_values_in_range(self):
        self.assertEqual(UTIL.number(5, 'x', 0, 10), 5)
        self.assertEqual(UTIL.number(2.5, 'x', 0, 10), 2.5)

    def test_number_rejects_bool_nan_and_out_of_range(self):
        for bad in (True, float('nan'), float('inf'), 11, -1, 'five'):
            with self.assertRaises(ValueError):
                UTIL.number(bad, 'x', 0, 10)

    def test_sha256_matches_known_digest(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'abc.bin'
            path.write_bytes(b'abc')
            self.assertEqual(UTIL.sha256(path),
                             'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad')

    def test_linked_is_false_for_ordinary_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'plain.txt'
            path.write_text('x')
            self.assertFalse(UTIL.linked(path))

    def test_copy_save_copies_ordinary_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'save'
            (source / 'Cache').mkdir(parents=True)
            (source / 'Cache' / 'data.bin').write_bytes(b'payload')
            destination = Path(temporary) / 'copy'
            UTIL.copy_save(source, destination)
            self.assertEqual((destination / 'Cache' / 'data.bin').read_bytes(), b'payload')


if __name__ == '__main__':
    unittest.main()
