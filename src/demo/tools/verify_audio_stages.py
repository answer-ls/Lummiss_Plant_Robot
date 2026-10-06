"""独立核对WAV头、MMR拆分以及抽样布局。"""
import json
from pathlib import Path
import struct
import wave


def read_wav(path, channels, rate):
    with wave.open(str(path), 'rb') as wav:
        if (wav.getnchannels(), wav.getframerate(), wav.getsampwidth()) != (channels, rate, 2):
            raise ValueError(f'{path.name}: WAV格式错误')
        data = wav.readframes(wav.getnframes())
    # 检查实际fmt块，不能仅凭文件名推断block_align。
    encoded = path.read_bytes()
    pos = 12
    while pos + 8 <= len(encoded):
        tag, length = struct.unpack_from('<4sI', encoded, pos)
        if tag == b'fmt ':
            fmt, ch, hz, byte_rate, align, bits = struct.unpack_from('<HHIIHH', encoded, pos + 8)
            if (fmt, ch, hz, byte_rate, align, bits) != (1, channels, rate, rate*channels*2, channels*2, 16):
                raise ValueError('WAV fmt/block_align错误')
            break
        pos += 8 + length + (length & 1)
    else:
        raise ValueError('没有WAV fmt块')
    return [v[0] for v in struct.iter_unpack('<h', data)]


def verify(folder):
    folder = Path(folder)
    raw = read_wav(folder / 'raw_4slot_24k.wav', 4, 24000)
    mmr = read_wav(folder / 'mmr_16k_3ch.wav', 3, 16000)
    if not raw or len(raw) % 12 or len(mmr) != len(raw) // 2:
        raise ValueError('raw/MMR样本数不满足4槽24k→3通道16k比例')
    report = dict(raw_header='4ch/24000Hz/16bit/block_align=8',
                  mmr_header='3ch/16000Hz/16bit/block_align=6',
                  resampler='esp_ae_rate_cvt：离线脚本未复现其内部滤波状态，不作逐样本重采样校验',
                  channels={})
    names = ['mic1', 'mic2', 'ref']
    for ch, slot in enumerate([0, 2, 1]):
        # 固件现用 esp_ae_rate_cvt；旧的 3:2 抽取/平均不能充当其预期输出。
        before = read_wav(folder / f'{names[ch]}_16k_before_afe.wav', 1, 16000)
        actual = mmr[ch::3]
        with wave.open(str(folder / f'{names[ch]}_16k_from_mmr.wav'), 'wb') as out:
            out.setnchannels(1)
            out.setsampwidth(2)
            out.setframerate(16000)
            out.writeframes(struct.pack(f'<{len(actual)}h', *actual))
        mismatch_split = sum(a != b for a, b in zip(before, actual)) + abs(len(before) - len(actual))
        report['channels'][names[ch]] = dict(source_slot=slot, samples=len(actual),
                                            mmr_split_mismatch=mismatch_split)
    metadata = json.loads((folder / 'stage_timestamps.json').read_text(encoding='utf-8'))
    sampled = metadata['frame_samples']
    report['layout_samples'] = len(sampled)
    report['layout_mismatch'] = sum(mmr[n*3:n*3+3] != [a, b, c] for n, a, b, c in sampled)
    report['output_samples'] = {name: len(read_wav(folder / f'{name}.wav', 1, 16000))
                                for name in ('afe_mono', 'uplink_pcm')}
    report['output_missing'] = any(n == 0 for n in report['output_samples'].values())
    report['note'] = 'raw与before_AFE同批；AFE/Opus采用同一采集窗口，但需根据延迟对齐，不将不同层同下标认定为同一源样本。'
    (folder / 'stage_verification.json').write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
    print(json.dumps(report, indent=2, ensure_ascii=False), flush=True)
    if len(sampled) != 100 or report['layout_mismatch'] or any(
            x['mmr_split_mismatch'] for x in report['channels'].values()):
        raise ValueError('逐级数据不一致，已保存stage_verification.json及所有WAV')
    return report


if __name__ == '__main__':
    import sys
    verify(Path(sys.argv[1]))
