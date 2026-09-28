import unittest
from analyze_h264_fragment import analyze


class FragmentTest(unittest.TestCase):
    def test_bridge_and_owner(self):
        lines = []
        for p, size, used in [(0x4ff86200, 50000, 0), (0x4ff92554, 8192, 1), (0x4ff94558, 70000, 0)]:
            lines.append(f'BLOCK heap=0x4ff861c0 end=0x4ffb6eff ptr=0x{p:x} size={size} used={used}')
        lines.append('CALLER block=0x4ff92554 depth=0 pc=0x48001234')
        _, blocks, candidates = analyze('\n'.join(lines))
        self.assertEqual(len(blocks), 3)
        self.assertEqual(candidates[0]['merged'], 128192)
        self.assertEqual(candidates[0]['callers'], ['0x48001234'])

    def test_no_merge_across_allocated_or_regions(self):
        text = '\n'.join([
            'BLOCK heap=0x4ff861c0 end=0x4ffb6eff ptr=0x4ff86200 size=50000 used=0',
            'BLOCK heap=0x4ff861c0 end=0x4ffb6eff ptr=0x4ff92554 size=8192 used=1',
            'BLOCK heap=0x4ff861c0 end=0x4ffb6eff ptr=0x4ff94558 size=8192 used=1',
            'BLOCK heap=0x50108080 end=0x50110000 ptr=0x50108100 size=20000 used=0'])
        _, _, candidates = analyze(text)
        self.assertEqual(len(candidates), 1)
        self.assertEqual(candidates[0]['merged'], 58192)

    def test_summary_cannot_generate_blocks(self):
        self.assertEqual(analyze('free 140771 allocated 57212')[1:], ([], []))


if __name__ == '__main__':
    unittest.main()
