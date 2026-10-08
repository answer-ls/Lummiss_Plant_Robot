"""验证PCM原样导出、四槽拆分和统计边界。"""
import array
import tempfile
import unittest
import wave
from pathlib import Path
from capture_hardware_audio import fnv, save_record, statistics


class CaptureTest(unittest.TestCase):
    def test_checksum_streaming(self):
        self.assertEqual(fnv(b'hello'), 0x4f9f2cab)
        self.assertEqual(fnv(b'lo', fnv(b'hel')), fnv(b'hello'))

    def test_statistics(self):
        s = statistics([0, 32767, -32768, 1])
        self.assertEqual(s['dc'], 0)
        self.assertEqual(s['peak'], 32768)
        self.assertEqual(s['clipping'], 2)
        self.assertEqual(s['zero_count'], 1)

    def test_four_slots_unchanged(self):
        import struct
        raw = b''.join(struct.pack('<hhhh', n, -n, n+100, 0) for n in range(200))
        meta = dict(bytes=len(raw), rate=16000, group=3)
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            save_record(folder, meta, raw)
            self.assertEqual((folder/'tdm_raw.bin').read_bytes(), raw)
            with wave.open(str(folder/'raw_4slot.wav')) as w:
                self.assertEqual((w.getnchannels(), w.getframerate(), w.getsampwidth()), (4,16000,2))
                self.assertEqual(w.readframes(200), raw)
            for ch in range(4):
                with wave.open(str(folder/f'slot{ch}.wav')) as w:
                    expected = b''.join(raw[n*8+ch*2:n*8+ch*2+2] for n in range(200))
                    self.assertEqual(w.readframes(200), expected)
            with self.assertRaises(ValueError):
                save_record(folder, meta, raw[:-1])


if __name__ == '__main__':
    unittest.main()
