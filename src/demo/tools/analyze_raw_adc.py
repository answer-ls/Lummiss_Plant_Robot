"""离线统计原始四槽，不改变 WAV；低频比例剔除 DC，另列 DC offset。"""
import argparse
import json
from pathlib import Path
import wave
import numpy as np
from scipy.signal import welch, find_peaks


def metrics(path):
    with wave.open(str(path), 'rb') as wav:
        assert wav.getnchannels() == 1 and wav.getsampwidth() == 2
        rate = wav.getframerate()
        x = np.frombuffer(wav.readframes(wav.getnframes()), dtype='<i2').astype(float)
    result = {}
    # 前两秒为安静段；全段和安静段分别统计，避免说话内容差异淹没底噪。
    for name, segment in [('all', x), ('quiet_first_2s', x[:rate * 2])]:
        f, p = welch(segment, rate, nperseg=min(24000, len(segment)), detrend='constant')
        peaks, _ = find_peaks(p)
        strongest = sorted(peaks, key=lambda i: p[i], reverse=True)[:6]
        total = p[f > 0].sum()
        result[name] = dict(rms=float(np.sqrt(np.mean(segment ** 2))),
                            dc_offset=float(segment.mean()), peak=float(abs(segment).max()),
                            below_300_percent=float(100 * p[(f > 0) & (f < 300)].sum() / total) if total else 0,
                            fft_peaks_hz=[float(f[i]) for i in strongest],
                            clipping_samples=int(np.count_nonzero((segment == -32768) | (segment == 32767))))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    result = {str(path.relative_to(args.directory)): metrics(path)
              for path in sorted(args.directory.glob('*/SLOT*_raw_24k.wav'))}
    if len(result) != 16:
        raise ValueError('必须有四组完整的16个单槽 WAV')
    (args.directory / 'metrics.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    for name, value in result.items():
        m = value['all']
        print(f"{name}: RMS={m['rms']:.1f} DC={m['dc_offset']:.1f} <300Hz={m['below_300_percent']:.1f}% peaks={m['fft_peaks_hz']}")


if __name__ == '__main__':
    main()
