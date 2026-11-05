"""Host oracles: unknown, unsupported and zero-coverage runs never become success."""
import unittest
from pathlib import Path
from run import judge, assert_owned_prefix, OUT


class Results(unittest.TestCase):
    def test_foreign_prefix_never_allowed(self):
        with self.assertRaises(RuntimeError):
            assert_owned_prefix(Path.home() / '.local/opt/msvc-wineprefix')
        assert_owned_prefix(OUT / 'build-prefix')

    def test_complete_roundtrip(self):
        self.assertEqual(judge({'status': 'PASS', 'frames': 24, 'verified': 24}, 0), 'PASS')

    def test_zero_coverage_is_failure(self):
        self.assertEqual(judge({'status': 'PASS', 'frames': 24, 'verified': 0}, 0), 'FAIL')

    def test_mismatched_pixels_cannot_pass(self):
        self.assertEqual(judge({'status': 'PASS', 'frames': 24, 'verified': 24, 'mismatches': 1}, 0), 'FAIL')

    def test_known_cycle_requires_fence_evidence(self):
        result = {'status': 'STALL', 'P': 1, 'N': 0, 'C': 0, 'B': 0}
        self.assertEqual(judge(result, 3, cycle=True), 'PASS')
        self.assertEqual(judge({}, 3, cycle=True), 'FAIL')
        self.assertEqual(judge(dict(result, N=1), 3, cycle=True), 'FAIL')

    def test_missing_evidence_is_failure(self):
        self.assertEqual(judge({}, 0), 'FAIL')

    def test_prerequisite_is_skip(self):
        self.assertEqual(judge({'status': 'SKIP', 'reason': 'no shared fences'}, 0), 'SKIP')

    def test_process_error_wins(self):
        self.assertEqual(judge({'status': 'PASS', 'frames': 24, 'verified': 24}, 1), 'FAIL')

    def test_stall_is_not_proven_cycle(self):
        self.assertEqual(judge({'status': 'STALL', 'reason': 'N not complete'}, 3), 'INCONCLUSIVE')

    def test_mutant_must_reach_oracle(self):
        self.assertEqual(judge({'status': 'FAIL', 'reason': 'pixel mismatch', 'mismatches': 20,
                                'frames': 24, 'verified': 24}, 1, mutant=True), 'PASS')
        self.assertEqual(judge({'status': 'FAIL', 'reason': 'device creation'}, 1, mutant=True), 'FAIL')


if __name__ == '__main__':
    unittest.main()
