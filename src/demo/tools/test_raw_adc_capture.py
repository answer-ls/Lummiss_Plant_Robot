"""验证导出完整性检查和四槽拆分，防止把导出错误误判为麦克风噪声。"""
import base64
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
import wave
from capture_raw_adc import Receiver, PHASES
from capture_audio_probe import checksum


class RawCaptureTest(unittest.TestCase):
    def test_complete_and_slot_bytes(self):
        # 四槽使用不同的正负数，检查大小端和交织顺序。
        frame = b'\x01\x00\xfe\xff\x34\x12\x00\x80'
        data = frame * 192000
        with tempfile.TemporaryDirectory() as folder, contextlib.redirect_stdout(io.StringIO()):
            receiver = Receiver(folder)
            for phase in sorted(PHASES):
                receiver.accept(f'RP_BEGIN {phase} 24000 4 {len(data)} 100 8000100')
                for offset in range(0, len(data), 240):
                    payload = base64.b64encode(data[offset:offset + 240]).decode()
                    receiver.accept(f'RP_DATA {phase} {offset} {payload}')
                receiver.accept(f'RP_END {phase} {checksum(data):08x}')
                for slot in range(4):
                    with wave.open(str(Path(folder) / phase / f'SLOT{slot}_raw_24k.wav'), 'rb') as wav:
                        self.assertEqual(wav.getnframes(), 192000)
                        self.assertEqual(wav.getframerate(), 24000)
                        self.assertEqual(wav.readframes(192000), frame[2*slot:2*slot+2] * 192000)
            self.assertTrue(receiver.accept('RP_DONE'))

    def test_reject_missing_or_corrupt_data(self):
        with tempfile.TemporaryDirectory() as folder, contextlib.redirect_stdout(io.StringIO()):
            receiver = Receiver(folder)
            receiver.accept('RP_BEGIN BASE 24000 4 1536000 0 8000000')
            with self.assertRaises(ValueError):
                receiver.accept('RP_DATA BASE 240 AAAA')
            with self.assertRaises(ValueError):
                receiver.accept('RP_END BASE 00000000')
            with self.assertRaises(ValueError):
                receiver.accept('RP_DONE')
            with self.assertRaises(ValueError):
                receiver.accept('RP_BEGIN BASE 24000 4 1536000 0 8000000')


if __name__ == '__main__':
    unittest.main()
