"""验证阈值判断，防止把释放总量误报为连续块改善。"""
import unittest
from analyze_rtc_contig import analyze


def row(tag, ts, free, largest):
    return f"MEM_CONTIG[{tag}] us={ts} INT={free}/{largest} DMA=10000/4096 PSRAM=2000000/1000000\n"


class ContigTest(unittest.TestCase):
    def test_free_increase_is_not_contiguous_improvement(self):
        result = analyze(row("WAKE_CLEANUP_DELETE_BEFORE", 1, 100000, 65536) +
                         row("WAKE_CLEANUP_DELETE_AFTER", 2, 108552, 65536))
        self.assertEqual(result["cleanup"][0]["free_delta"], 8552)
        self.assertEqual(result["cleanup"][0]["largest_delta"], 0)

    def test_threshold_crossing_and_order(self):
        result = analyze(row("AFTER", 2, 150000, 92223) + row("BEFORE", 1, 200000, 92224))
        self.assertEqual(result["crossings"][0]["from"]["tag"], "BEFORE")

    def test_already_below_is_not_a_measured_crossing(self):
        result = analyze(row("BOOT", 1, 190000, 65536))
        self.assertTrue(result["already_below_at_first_sample"])
        self.assertEqual(result["crossings"], [])

    def test_missing_before_does_not_invent_delta(self):
        result = analyze(row("WAKE_CLEANUP_DELETE_AFTER", 2, 150000, 100000))
        self.assertEqual(result["cleanup"], [])


if __name__ == "__main__":
    unittest.main()
