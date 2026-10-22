#!/usr/bin/env python3
import unittest
from native_frame_pacing import parse_pacing, pacing_gate


class PacingTests(unittest.TestCase):
    def test_submission_intervals_are_distinct_from_renderer_fps(self):
        text = ('[DESKBAR:SELECTOR] pid=12 process=MDGEARS.ELF\n'
                '[MESADEMOS] START Gears backend=gallium\n'
                '[MESADEMOS] RESULT fps=999\n'
                '[VISION:PACING] owner=12 intervals=100 hz=300 gap_max=30 gap_total=525 gaps=5:99,30:1,\n')
        run, = parse_pacing(text)
        self.assertEqual(run['scene'], 'Gears')
        self.assertEqual(run['backend'], 'gallium')
        self.assertAlmostEqual(run['submission_fps'], 100 * 300 / 525)
        self.assertAlmostEqual(run['p99_ms'], 1000 / 60)
        self.assertEqual(run['max_ms'], 100)
        self.assertEqual(run['over_50ms'], 1)

    def test_saturation_keeps_exact_max_and_total(self):
        run, = parse_pacing('[VISION:PACING] owner=7 intervals=2 hz=100 gap_max=1200 gap_total=1202 gaps=2:1,1000:1,\n')
        self.assertEqual(run['max_ms'], 12000)
        self.assertAlmostEqual(run['submission_fps'], 200 / 1202)

    def test_truncated_histogram_is_not_accepted(self):
        with self.assertRaises(ValueError):
            parse_pacing('[VISION:PACING] owner=1 intervals=100 hz=300 gaps=5:90,\n')

    def test_no_trace_is_not_a_measurement(self):
        self.assertEqual(parse_pacing('[MESADEMOS] RESULT fps=200'), [])
        self.assertFalse(pacing_gate([]))

    def test_good_average_cannot_hide_stalls(self):
        run = dict(application='GLMARK2.ELF', intervals=1000, p95_ms=30,
                   p99_ms=120, over_100ms=20, submission_fps=200)
        self.assertFalse(pacing_gate([run]))
        run.update(p99_ms=45, over_100ms=1)
        self.assertTrue(pacing_gate([run]))
        self.assertFalse(pacing_gate([run], mesa_demos=True))


if __name__ == '__main__':
    unittest.main()
