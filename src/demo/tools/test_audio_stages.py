"""验证串口接收、WAV 布局和 MMR 拆分；不伪造 esp_ae_rate_cvt 的离线结果。"""
import base64
import contextlib
import io
from pathlib import Path
import struct
import tempfile
import unittest

from capture_audio_probe import Receiver, checksum, save_wav
from verify_audio_stages import verify


class StageTest(unittest.TestCase):
    def test_receiver_and_mmr_split(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            p = Path(temp)
            raw = struct.pack('<6000h', *[
                ((n * 79 + slot * 37) % 2000) - 1000
                for n in range(1500) for slot in range(4)
            ])
            # 测试只验证交错布局；这里的单通道值不是重采样算法的黄金输出。
            mmr = struct.pack('<3000h', *[
                ((n * 53 + ch * 29) % 2000) - 1000
                for n in range(1000) for ch in range(3)
            ])
            mono = [b''.join(mmr[pos:pos + 2] for pos in range(ch * 2, len(mmr), 6))
                    for ch in range(3)]
            streams = [raw, mono[0], mono[0], mmr, *mono]
            receiver = Receiver(p)
            for n in range(0, 1000, 10):
                a, b, c = struct.unpack_from('<hhh', mmr, n * 6)
                receiver.accept(f'AP_FRAME {n} {a} {b} {c}')
            for ident, data in enumerate(streams):
                receiver.accept(f'AP_BEGIN {ident} {24000 if ident == 0 else 16000} '
                                f'{[4, 1, 1, 3, 1, 1, 1][ident]} {len(data)} 100 200')
                for offset in range(0, len(data), 256):
                    encoded = base64.b64encode(data[offset:offset + 256]).decode()
                    receiver.accept(f'AP_DATA {ident} {offset} {encoded}')
                receiver.accept(f'AP_END {ident} {checksum(data):08x}')
            self.assertTrue(receiver.accept('AP_DONE'))
            self.assertEqual(verify(p)['channels']['mic1']['mmr_split_mismatch'], 0)
            damaged = bytearray(mono[1])
            damaged[10] ^= 1
            save_wav(p / 'mic2_16k_before_afe.wav', damaged, 16000, 1)
            with self.assertRaises(ValueError):
                verify(p)
            save_wav(p / 'raw_4slot_24k.wav', raw, 24000, 1)
            with self.assertRaises(ValueError):
                verify(p)


if __name__ == '__main__':
    unittest.main()
