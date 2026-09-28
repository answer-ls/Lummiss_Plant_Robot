"""实际C重采样→串口接收→WAV→PC独立复算，覆盖负数舍入与通道串扰。"""
import base64
import contextlib
import io
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from capture_audio_probe import Receiver, checksum, save_wav
from verify_audio_stages import verify


class StageTest(unittest.TestCase):
    def test_real_resampler_and_receiver(self):
        source = (Path(__file__).resolve().parents[1] / 'components/xiaozhi_audio/xiaozhi_audio.c').read_text(encoding='utf-8')
        start = source.index('static void resample_mic_24k_to_16k(')
        function = source[start:source.index('\n}\n', start) + 3]
        with tempfile.TemporaryDirectory() as temp, contextlib.redirect_stdout(io.StringIO()):
            p = Path(temp)
            c = '#include <stdint.h>\n#include <stddef.h>\n#include <stdio.h>\n#define XIAOZHI_AFE_CHANNELS 3\n#define XIAOZHI_CAPTURE_CHANNELS 4\n' + function + r'''
int main(int argc, char **argv) {
    int16_t raw[6000], out[3000];
    FILE *f=fopen(argv[1],"rb");
    if (!f || fread(raw,2,6000,f)!=6000) return 1;
    fclose(f);
    resample_mic_24k_to_16k(raw,out,1000);
    f=fopen(argv[2],"wb");
    if (!f || fwrite(out,2,3000,f)!=3000) return 2;
    fclose(f);return 0;
}
'''
            (p/'test.c').write_text(c, encoding='utf-8')
            subprocess.run(['D:/mingw64/bin/gcc.exe', str(p/'test.c'), '-o', str(p/'test.exe')], check=True, capture_output=True)
            raw = struct.pack('<6000h', *[((n*7919 + slot*3571) % 65536)-32768 for n in range(1500) for slot in range(4)])
            (p/'raw.bin').write_bytes(raw)
            subprocess.run([str(p/'test.exe'), str(p/'raw.bin'), str(p/'mmr.bin')], check=True)
            mmr = (p/'mmr.bin').read_bytes()
            mono = [b''.join(mmr[pos:pos+2] for pos in range(ch*2,len(mmr),6)) for ch in range(3)]
            streams = [raw, mono[0], mono[0], mmr, *mono]
            r = Receiver(p)
            for n in range(0,1000,10):
                a,b,c = struct.unpack_from('<hhh',mmr,n*6)
                r.accept(f'AP_FRAME {n} {a} {b} {c}')
            for ident,data in enumerate(streams):
                r.accept(f'AP_BEGIN {ident} {24000 if ident==0 else 16000} {[4,1,1,3,1,1,1][ident]} {len(data)} 100 200')
                for offset in range(0,len(data),256):
                    r.accept(f'AP_DATA {ident} {offset} {base64.b64encode(data[offset:offset+256]).decode()}')
                r.accept(f'AP_END {ident} {checksum(data):08x}')
            self.assertTrue(r.accept('AP_DONE'))
            self.assertEqual(verify(p)['channels']['mic1']['samples'],1000)
            damaged = bytearray(mono[1]); damaged[10] ^= 1
            save_wav(p/'mic2_16k_before_afe.wav',damaged,16000,1)
            with self.assertRaises(ValueError): verify(p)
            save_wav(p/'raw_4slot_24k.wav',raw,24000,1)
            with self.assertRaises(ValueError): verify(p)


if __name__ == '__main__':
    unittest.main()
