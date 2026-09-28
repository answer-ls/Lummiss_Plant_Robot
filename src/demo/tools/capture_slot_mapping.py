"""接收物理槽位定位：分别保存固件拆槽与PC拆槽，逐样本核对。"""
import argparse
import base64
import csv
import json
from pathlib import Path
import re
import struct
import time
from capture_audio_probe import checksum, save_wav

PHASES = {'MIC1_NEAR', 'MIC2_NEAR', 'PLAYBACK_1K'}
SIZE = 1536000


def compare_and_save(folder, raw, firmware):
    if len(raw) != SIZE or len(firmware) != SIZE:
        raise ValueError('录音长度不符')
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'tdm_raw.bin').write_bytes(raw)
    (folder / 'firmware_planar.bin').write_bytes(firmware)
    save_wav(folder / 'raw_4slot_24k.wav', raw, 24000, 4)
    # PC用明确的小端四个int16解析；不复用固件的索引算法。
    slots = [bytearray() for _ in range(4)]
    for frame in struct.iter_unpack('<hhhh', raw):
        for slot, value in enumerate(frame):
            slots[slot].extend(struct.pack('<h', value))
    results = {}
    for slot, offline in enumerate(slots):
        actual = firmware[slot * (SIZE // 4):(slot + 1) * (SIZE // 4)]
        save_wav(folder / f'slot{slot}.wav', actual, 24000, 1)
        save_wav(folder / f'slot{slot}_pc.wav', offline, 24000, 1)
        different = [i for i, (a, b) in enumerate(zip(struct.iter_unpack('<h', actual),
                                                     struct.iter_unpack('<h', offline))) if a != b]
        results[f'slot{slot}'] = dict(samples=SIZE // 8, mismatch_samples=len(different),
                                      first_mismatch=different[0] if different else None)
    with (folder / 'first_1000_frames.csv').open('w', newline='', encoding='utf-8') as out:
        writer = csv.writer(out)
        writer.writerow(['frame', 'byte_offset', 'slot0', 'slot1', 'slot2', 'slot3'])
        for n, frame in enumerate(struct.iter_unpack('<hhhh', raw[:8000])):
            writer.writerow([n, n * 8, *frame])
    (folder / 'split_comparison.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    return results


class Receiver:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.streams = {}
        self.results = {}

    def accept(self, line):
        match = re.search(r'RM_(BEGIN|DATA|END|DONE)\b(.*)', line)
        if not match:
            return False
        kind, tail = match.groups()
        f = tail.split()
        if kind == 'DONE':
            expected = {(p, k) for p in PHASES for k in ('RAW', 'FW')}
            if set(self.streams) != expected or not all(x['done'] for x in self.streams.values()):
                raise ValueError('数据不完整')
            if any(s['mismatch_samples'] for phase in self.results.values() for s in phase.values()):
                raise ValueError('固件与PC拆分不一致！已保留原始数据和差异报告')
            return True
        key = tuple(f[:2])
        if key[0] not in PHASES or key[1] not in ('RAW', 'FW'):
            raise ValueError('未知组别')
        if kind == 'BEGIN':
            rate, channels, size, start, end = map(int, f[2:])
            if key in self.streams or (rate, channels, size) != (24000, 4, SIZE):
                raise ValueError('重复录音或格式不符')
            self.streams[key] = dict(data=bytearray(), done=False, start_us=start, end_us=end)
            print(f'接收 {key}', flush=True)
        elif kind == 'DATA':
            item = self.streams[key]
            if item['done'] or int(f[2]) != len(item['data']):
                raise ValueError('串口丢行/重复数据')
            item['data'].extend(base64.b64decode(f[3], validate=True))
            if len(item['data']) > SIZE:
                raise ValueError('数据过长')
        elif kind == 'END':
            item = self.streams[key]
            if item['done'] or len(item['data']) != SIZE or checksum(item['data']) != int(f[2], 16):
                raise ValueError('长度或校验失败')
            item['done'] = True
            folder = self.directory / key[0]
            folder.mkdir(exist_ok=True, parents=True)
            (folder / ('tdm_raw.bin' if key[1] == 'RAW' else 'firmware_planar.bin')).write_bytes(item['data'])
            pair = [self.streams.get((key[0], k)) for k in ('RAW', 'FW')]
            if all(x and x['done'] for x in pair):
                self.results[key[0]] = compare_and_save(folder, pair[0]['data'], pair[1]['data'])
                (folder / 'timing.json').write_text(json.dumps({k: v for k, v in pair[0].items() if k != 'data'}, indent=2), encoding='utf-8')
                print(f'{key[0]} 拆分比较：{self.results[key[0]]}', flush=True)
        return False


def main():
    import serial
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    args = parser.parse_args()
    directory = Path(__file__).resolve().parents[3] / 'logs' / time.strftime('slot_mapping_%Y%m%d_%H%M%S')
    directory.mkdir(parents=True, exist_ok=False)
    receiver = Receiver(directory)
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, 115200, 2
    port.dtr = port.rts = False
    port.open()
    print(f'{directory}\n现在按P4 RESET，按提示在物理MIC1/MIC2附近发声，第三组自动播放1kHz。', flush=True)
    deadline = time.monotonic() + 2700
    with port, (directory / 'serial.log').open('w', encoding='utf-8') as log:
        while time.monotonic() < deadline:
            line = port.readline().decode('utf-8', errors='replace')
            if not line:
                continue
            log.write(line)
            if 'RM_DATA ' not in line:
                log.flush()
                print(line.rstrip(), flush=True)
            if 'RAW_ADC: RESULT=' in line and 'RESULT=ESP_OK' not in line:
                raise RuntimeError('板端测试失败，查看serial.log')
            if receiver.accept(line):
                print('完成：三组四槽逐样本一致。物理映射仍需比较各组响应。', flush=True)
                return
    raise TimeoutError('未收到完整数据；保留日志，不作为通过结果')


if __name__ == '__main__':
    main()
