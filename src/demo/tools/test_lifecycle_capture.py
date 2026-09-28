"""验证生命周期导出校验、空/短状态记录及四槽拆分。"""
import base64
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
import wave
from capture_lifecycle import Receiver
from capture_audio_probe import checksum


class CaptureTest(unittest.TestCase):
    def test_windows(self):
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            p=Path(temp);r=Receiver(p)
            r.accept('LC_EVENT 2000 LISTENING')
            raw=b'\x01\x00\xfe\xff\x00\x80\xff\x7f'*24000
            for i in range(9):
                data=raw if i==0 else b''
                r.accept(f'LC_BEGIN {i} STATE{i} {len(data)} 1000 2000 1002000 1 0')
                r.accept(f'LC_SNAPSHOT {i} BEFORE 1200 '+ '00000000 '*10)
                r.accept(f'LC_REG {i} BEFORE 43 26')
                for offset in range(0,len(data),240):
                    r.accept(f'LC_DATA {i} {offset} '+base64.b64encode(data[offset:offset+240]).decode())
                r.accept(f'LC_END {i} {checksum(data):08x}')
            self.assertTrue(r.accept('LC_DONE'))
            meta=json.loads((p/'lifecycle.json').read_text())
            self.assertEqual(meta['records']['0']['snapshots']['BEFORE']['regs']['43'],26)
            self.assertEqual(meta['records']['1']['size'],0)
            for slot in range(4):
                with wave.open(str(p/f'STATE0/slot{slot}.wav'),'rb') as w:
                    self.assertEqual(w.getnframes(),24000)
                    self.assertEqual(w.readframes(24000),raw[slot*2:slot*2+2]*24000)
    def test_missing_data(self):
        with tempfile.TemporaryDirectory() as temp:
            r=Receiver(temp);r.accept('LC_BEGIN 0 TEST 8 0 0 0 0 0')
            with self.assertRaises(ValueError):r.accept('LC_DATA 0 8 AAAAAAAA')
            with self.assertRaises(ValueError):r.accept('LC_END 0 00000000')
            with self.assertRaises(ValueError):r.accept('LC_DONE')

if __name__=='__main__':unittest.main()
