"""接收四组 I2S 原始录音；不复位、不烧录，不依赖 SD 卡。"""
import argparse
import base64
import json
from pathlib import Path
import re
import time

from capture_audio_probe import checksum, save_wav

PHASES = {'BASE', 'A_MIC1', 'B_MIC2', 'C_MIC12'}


class Receiver:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.streams = {}

    def accept(self, line):
        match = re.search(r'RP_(BEGIN|DATA|END|DONE)\b(.*)', line)
        if not match:
            return False
        kind, tail = match.groups()
        fields = tail.split()
        if kind == 'BEGIN':
            phase = fields[0]
            rate, channels, size, start, end = map(int, fields[1:])
            if phase not in PHASES or phase in self.streams:
                raise ValueError('重复或未知组别，可能设备重启')
            if (rate, channels, size) != (24000, 4, 1536000):
                raise ValueError('原始格式或采集长度不符')
            self.streams[phase] = dict(rate=rate, channels=channels, size=size,
                                      start_us=start, end_us=end, data=bytearray(), done=False)
            print(f'正在导出 {phase}，请等待', flush=True)
        elif kind == 'DATA':
            phase, offset, encoded = fields
            item = self.streams[phase]
            if item['done'] or int(offset) != len(item['data']):
                raise ValueError('串口丢行或重复数据')
            item['data'].extend(base64.b64decode(encoded, validate=True))
            if len(item['data']) > item['size']:
                raise ValueError('数据超过预期长度')
        elif kind == 'END':
            phase, digest = fields
            item = self.streams[phase]
            if item['done'] or len(item['data']) != item['size'] or checksum(item['data']) != int(digest, 16):
                raise ValueError('长度或 FNV 校验失败，不能使用本组录音')
            folder = self.directory / phase
            folder.mkdir(parents=True, exist_ok=False)
            data = item['data']
            save_wav(folder / 'raw_4slot_24k.wav', data, 24000, 4)
            # 仅按字节拆槽；不滤波、不增益、不重采样，不预先假设槽位对应哪只麦克风。
            for slot in range(4):
                mono = bytearray(len(data) // 4)
                mono[0::2] = data[slot * 2::8]
                mono[1::2] = data[slot * 2 + 1::8]
                save_wav(folder / f'SLOT{slot}_raw_24k.wav', mono, 24000, 1)
            item['done'] = True
            (folder / 'timing.json').write_text(json.dumps(
                {k: v for k, v in item.items() if k != 'data'}, indent=2), encoding='utf-8')
            print(f'{phase} 校验通过，四槽 WAV 已保存', flush=True)
        else:
            if set(self.streams) != PHASES or not all(x['done'] for x in self.streams.values()):
                raise ValueError('四组录音不完整')
            return True
        return False


def main():
    import serial
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    args = parser.parse_args()
    directory = Path(__file__).resolve().parents[3] / 'logs' / time.strftime('raw_adc_%Y%m%d_%H%M%S')
    directory.mkdir(parents=True, exist_ok=False)
    receiver = Receiver(directory)
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, 115200, 2
    port.dtr = port.rts = False
    port.open()
    print(f'保存位置：{directory}\n现在按 P4 RESET；按 PREPARE/RECORD 提示录音。导出约13分钟。', flush=True)
    deadline = time.monotonic() + 1800
    with port, (directory / 'serial.log').open('w', encoding='utf-8') as log:
        while time.monotonic() < deadline:
            line = port.readline().decode('utf-8', errors='replace')
            if not line:
                continue
            log.write(line)
            if 'RP_DATA ' not in line:
                log.flush()
                print(line.rstrip(), flush=True)
            if 'RAW_ADC: RESULT=' in line and 'RESULT=ESP_OK' not in line:
                raise RuntimeError('板端采集失败，请检查 serial.log')
            if receiver.accept(line):
                print(f'四组完整导出：{directory}', flush=True)
                return
    raise TimeoutError('采集/导出未完成，保留 serial.log；不要使用不完整录音下结论')


if __name__ == '__main__':
    main()
