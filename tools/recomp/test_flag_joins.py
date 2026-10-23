"""A je/jne reached across a label must test the flags its producer set.

The lifter's fallback `_flags` is never assigned, so the branch used to be
dead. These cases pin the rewrite: a cmp reaching the branch through a label is
materialized as `_zf`, and a branch with no producer is left and reported.
"""
import tempfile
import unittest
from pathlib import Path

from .flag_joins import fix_flag_joins

JOIN = """void sub_00001000(void)
{
    int _flags = 0; /* fallback flag var */
    _fa = eax; _fb = ecx; /* cmp eax, ecx */
loc_00001010: ;
    if (_flags /* je: equal / zero */) goto loc_00001020;
    return;
loc_00001020: ;
    return;
}
"""

ENTRY = """void sub_00002000(void)
{
    int _flags = 0; /* fallback flag var */
    if (_flags /* je: equal / zero */) goto loc_00002010;
    return;
loc_00002010: ;
    return;
}
"""


class FlagJoinTest(unittest.TestCase):

    def run_pass(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'recomp_0000.c'
            path.write_text(text)
            fixed, unfixed = fix_flag_joins(Path(tmp))
            return fixed, unfixed, path.read_text()

    def test_cmp_across_a_label_is_materialized(self):
        fixed, unfixed, out = self.run_pass(JOIN)
        self.assertEqual((fixed, unfixed), (1, []))
        self.assertIn('int _flags = 0; /* fallback flag var */ int _zf = 0;', out)
        self.assertIn('_fa = eax; _fb = ecx; /* cmp eax, ecx */\n    _zf = (_fa == _fb);', out)
        self.assertIn('if (_zf) goto loc_00001020;', out)
        self.assertNotIn('_flags /* je', out)

    def test_entry_without_a_producer_is_left_and_reported(self):
        fixed, unfixed, out = self.run_pass(ENTRY)
        self.assertEqual(fixed, 0)
        self.assertEqual(len(unfixed), 1)
        self.assertTrue(unfixed[0][4].startswith('blocked at'), unfixed[0][4])
        self.assertIn('if (_flags /* je: equal / zero */) goto loc_00002010;', out)


if __name__ == '__main__':
    unittest.main()
