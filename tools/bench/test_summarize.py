import contextlib
import csv
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('bench_summary', Path(__file__).resolve().parent / 'summarize.py')
BENCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BENCH)


class BenchmarkSummaryTests(unittest.TestCase):
    def make_run(self, folder, present=True):
        (folder / 'windows.json').write_text(json.dumps([
            {'name': 'race', 'start': 100, 'end': 500, 'cpu_ms': 320, 'logical_processors': 16}
        ]), encoding='utf-8-sig')
        with (folder / 'frames.csv').open('w', newline='') as stream:
            writer = csv.writer(stream)
            writer.writerow(['qpc_ms', 'frame_ms', 'present_qpc_ms', 'present_ms'])
            # This straddling interval must not count as a hitch within the window.
            writer.writerow([110, 100, 110 if present else '', 100 if present else ''])
            for timestamp in range(130, 531, 20):
                writer.writerow([timestamp, 20, timestamp - 2 if present else '', 20 if present else ''])

    def test_separate_host_metrics_and_interval_boundaries(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            self.make_run(folder)
            with contextlib.redirect_stdout(io.StringIO()):
                results = BENCH.summarize_run(folder)
            self.assertEqual(results[0]['frame_ms']['frames'], 19)
            self.assertEqual(results[0]['present_ms']['frames'], 19)
            self.assertEqual(results[0]['frame_ms']['fps'], 50)
            self.assertEqual(results[0]['frame_ms']['hitch'], 0)
            self.assertEqual(results[0]['process_cpu_percent'], 5)
            self.assertEqual(json.loads((folder / 'summary.json').read_text()), results)

    def test_missing_host_samples_rejects_run(self):
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            self.make_run(folder, present=False)
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(ValueError, 'insufficient present_ms'):
                BENCH.summarize_run(folder)
            self.assertFalse((folder / 'summary.json').exists())


if __name__ == '__main__':
    unittest.main()
