"""验证串口提示不会被误当协议，同时保留缺块和校验失败保护。"""
import json
from pathlib import Path
import tempfile
import unittest

from capture_jpeg_failure import Receiver, checksum


class ReceiverTest(unittest.TestCase):
    def setUp(self):
        # 测试产物也统一放在项目根目录 logs 下。
        logs = Path(__file__).resolve().parents[3] / 'logs'
        logs.mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='jpeg_receiver_test_', dir=logs)
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.receiver = Receiver(self.directory)
        self.data = b'\xff\xd8\x00\x01\xff\xd9'
        self.metadata = dict(size=len(self.data), before_hash=checksum(self.data),
                             after_hash=checksum(self.data), mismatch=0, first_diff=-1)

    def begin(self):
        self.receiver.accept('JP_BEGIN ' + json.dumps(self.metadata) + '\r\n')

    def test_capture_hint_is_not_done(self):
        line = ('W (48906) JPEG_PROBE: CAPTURED sequence=6 bytes=71974 '
                'ret=ESP_ERR_INVALID_STATE mismatch=0 first_diff=-1；'
                '请停止RTC等待JP_DONE，勿复位\r\n')
        self.assertFalse(self.receiver.accept(line))
        self.assertIsNone(self.receiver.metadata)
        self.begin()
        self.assertFalse(self.receiver.accept(line))
        for stream in range(2):
            self.receiver.accept(f'JP_DATA {stream} 0 {self.data.hex()}\r\n')
        self.assertTrue(self.receiver.accept('JP_DONE\r\n'))
        self.assertEqual((self.directory / 'before.jpg').read_bytes(), self.data)
        self.assertEqual((self.directory / 'after.jpg').read_bytes(), self.data)

    def test_real_done_without_begin_is_rejected(self):
        with self.assertRaisesRegex(ValueError, '缺少BEGIN'):
            self.receiver.accept('JP_DONE\r\n')

    def test_missing_chunk_is_rejected(self):
        self.begin()
        with self.assertRaisesRegex(ValueError, '缺块'):
            self.receiver.accept('JP_DATA 0 256 ff')

    def test_checksum_failure_is_rejected(self):
        self.begin()
        for stream in range(2):
            self.receiver.accept(f'JP_DATA {stream} 0 {bytes(len(self.data)).hex()}')
        with self.assertRaisesRegex(ValueError, '校验失败'):
            self.receiver.accept('JP_DONE')
        self.assertFalse((self.directory / 'before.jpg').exists())


if __name__ == '__main__':
    unittest.main()
