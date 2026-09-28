"""离线检查串口完整性与无效窗口判定，不伪造实机测量。"""
import base64
import tempfile
import unittest
import wave
from pathlib import Path
from capture_uplink_ab import Capture, checksum


class ReceiverTest(unittest.TestCase):
    def setUp(self):
        logs = Path(__file__).resolve().parents[3] / 'logs'
        logs.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(dir=logs)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.capture = Capture(self.root)

    def begin(self, phase, crossed=0):
        self.capture.line(f'CD_BEGIN {phase} 16 1000 4001000 {crossed} 0 0 3 1 80')

    def data(self, phase):
        data = bytes(range(16))
        self.capture.line(f'CD_DATA {phase} 0 {base64.b64encode(data).decode()}')
        self.capture.line(f'CD_END {phase} {checksum(data):08x}')

    def test_roundtrip_and_crossed(self):
        self.begin(0); self.data(0)
        self.begin(1, crossed=1); self.data(1)
        self.assertTrue(self.capture.line('CD_DONE'))
        self.assertTrue(self.capture.phases[0]['valid'])
        self.assertFalse(self.capture.phases[1]['valid'])
        self.assertEqual(self.capture.phases[0]['rx_overflow_count'], 3)
        self.assertEqual((self.root/'A_UDP'/'tdm_raw.bin').read_bytes(), bytes(range(16)))
        with wave.open(str(self.root/'A_UDP'/'raw_4slot_24k.wav')) as wav:
            self.assertEqual((wav.getnchannels(), wav.getframerate(), wav.getsampwidth()), (4,24000,2))
            self.assertEqual(wav.readframes(2), bytes(range(16)))
        with wave.open(str(self.root/'A_UDP'/'slot2_24k.wav')) as wav:
            self.assertEqual(wav.getnchannels(), 1)
            self.assertEqual(wav.readframes(2), bytes([4,5,12,13]))

    def test_missing_chunk(self):
        self.begin(0)
        with self.assertRaises(ValueError):
            self.capture.line('CD_DATA 0 240 AAAA')

    def test_bad_checksum(self):
        self.begin(0)
        self.capture.line('CD_DATA 0 0 ' + base64.b64encode(bytes(range(16))).decode())
        with self.assertRaises(ValueError):
            self.capture.line('CD_END 0 00000000')

    def test_missing_phase(self):
        self.begin(0); self.data(0)
        with self.assertRaises(ValueError):
            self.capture.line('CD_DONE')


if __name__ == '__main__':
    unittest.main()
