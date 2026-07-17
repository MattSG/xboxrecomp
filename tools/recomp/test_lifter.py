import unittest

from .lifter import _emit_cond_goto


class ConditionalTailJumpTests(unittest.TestCase):
    def test_external_target_publishes_frame_pointer(self):
        class FakeLifter:
            @staticmethod
            def _is_external_target(_target): return True

            @staticmethod
            def _call_target_name(_target): return "sub_0008587E"

        result = _emit_cond_goto("eax != 0", "jne", "not zero",
                                 0x8587E, FakeLifter())
        self.assertIn("g_seh_ebp = ebp; sub_0008587E(); return;", result)


if __name__ == "__main__":
    unittest.main()
