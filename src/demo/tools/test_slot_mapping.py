"""编译固件实际拆槽函数，用PC独立解析核对全部样本，并注入一处错误。"""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from capture_slot_mapping import compare_and_save, SIZE, Receiver


class MappingTest(unittest.TestCase):
    def test_actual_c_split_and_corruption(self):
        source = (Path(__file__).resolve().parents[1] / 'components/xiaozhi_audio/raw_adc_probe.c').read_text(encoding='utf-8')
        function = source[source.index('static void split_slots('):source.index('static void export_data(')]
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            c = '#include <stdint.h>\n#include <stdlib.h>\n#include <stdio.h>\n#define RAW_BYTES 1536000\n' + function + r'''
int main(int argc, char **argv) {
    int16_t *raw=malloc(RAW_BYTES), *out=malloc(RAW_BYTES);
    FILE *f=fopen(argv[1],"rb");
    if (!f || fread(raw,1,RAW_BYTES,f)!=RAW_BYTES) return 1;
    fclose(f);
    for (size_t n=0;n<RAW_BYTES/8;n+=240) split_slots(raw+4*n,out,n,240);
    f=fopen(argv[2],"wb");
    if (!f || fwrite(out,1,RAW_BYTES,f)!=RAW_BYTES) return 2;
    fclose(f); free(raw); free(out); return 0;
}
'''
            (folder / 'split.c').write_text(c, encoding='utf-8')
            subprocess.run(['D:/mingw64/bin/gcc.exe', '-Wall', '-Werror', str(folder / 'split.c'), '-o', str(folder / 'split.exe')], check=True, capture_output=True)
            raw = b''.join(struct.pack('<hhhh', *[((n * 37 + slot * 1031) % 65536) - 32768 for slot in range(4)]) for n in range(SIZE // 8))
            (folder / 'raw.bin').write_bytes(raw)
            subprocess.run([str(folder / 'split.exe'), str(folder / 'raw.bin'), str(folder / 'fw.bin')], check=True)
            fw = (folder / 'fw.bin').read_bytes()
            report = compare_and_save(folder / 'valid', raw, fw)
            self.assertTrue(all(x['mismatch_samples'] == 0 for x in report.values()))
            broken = bytearray(fw)
            broken[SIZE // 4 + 1000 * 2] ^= 1
            report = compare_and_save(folder / 'broken', raw, broken)
            self.assertEqual(report['slot1']['mismatch_samples'], 1)
            self.assertEqual(report['slot1']['first_mismatch'], 1000)
            self.assertEqual(len((folder / 'valid/first_1000_frames.csv').read_text().splitlines()), 1001)

    def test_incomplete_export_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            r = Receiver(directory)
            with self.assertRaises(ValueError):
                r.accept('RM_DONE')
            r.accept('RM_BEGIN MIC1_NEAR RAW 24000 4 1536000 0 8000000')
            with self.assertRaises(ValueError):
                r.accept('RM_DATA MIC1_NEAR RAW 240 AAAA')
            with self.assertRaises(ValueError):
                r.accept('RM_END MIC1_NEAR RAW 00000000')


if __name__ == '__main__':
    unittest.main()
