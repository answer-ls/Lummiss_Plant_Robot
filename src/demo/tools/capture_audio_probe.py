"""接收单次音频诊断，校验完整性后生成 WAV；不复位、不烧录设备。"""
import argparse
import base64
import json
from pathlib import Path
import re
import time
import wave


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def save_wav(path, data, rate, channels):
    with wave.open(str(path), 'wb') as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(data)


class Receiver:
    def __init__(self, directory):
        self.directory = directory
        self.streams = {}
        self.frames = []
        self.timestamps = []

    def accept(self, line):
        marker = re.search(r'AP_(FRAME|TIME)\b(.*)', line)
        if marker:
            (self.frames if marker[1] == 'FRAME' else self.timestamps).append(list(map(int, marker[2].split())))
            return False
        match = re.search(r'AP_(BEGIN|DATA|END|DONE)\b(.*)', line)
        if not match:
            return False
        kind, tail = match.groups()
        fields = tail.split()
        if kind == 'BEGIN':
            ident, rate, channels, size, first, last = map(int, fields)
            if ident in self.streams:
                raise ValueError('重复录音/设备重启，请重新接收')
            self.streams[ident] = dict(rate=rate, channels=channels, size=size,
                                       first_us=first, last_us=last, data=bytearray(), done=False)
            print(f'接收通道组 {ident}: {size} bytes', flush=True)
        elif kind == 'DATA':
            ident, position = map(int, fields[:2])
            item = self.streams[ident]
            if position != len(item['data']):
                raise ValueError(f'串口丢行: stream={ident} offset={position}')
            item['data'].extend(base64.b64decode(fields[2], validate=True))
            if len(item['data']) > item['size']:
                raise ValueError('接收长度超过声明长度')
        elif kind == 'END':
            ident = int(fields[0])
            item = self.streams[ident]
            data = item['data']
            if len(data) != item['size'] or checksum(data) != int(fields[1], 16):
                raise ValueError('音频为空、长度不符或校验失败，不能当作有效录音')
            if len(data) % (2 * item['channels']):
                raise ValueError('PCM 帧长度错误')
            item['done'] = True
        else:
            if set(self.streams) not in ({0, 1}, {0, 1, 2}, set(range(7))) or not all(s['done'] for s in self.streams.values()):
                raise ValueError('音频组不完整')
            for ident, item in self.streams.items():
                data = item['data']
                if not data:
                    print(f'警告：通道组 {ident} 没有捕获到样本，空WAV不能用于评价音质', flush=True)
                save_wav(self.directory / {0: 'raw_4slot_24k.wav', 1: 'afe_mono.wav', 2: 'uplink_pcm.wav', 3: 'mmr_16k_3ch.wav', 4: 'mic1_16k_before_afe.wav', 5: 'mic2_16k_before_afe.wav', 6: 'ref_16k_before_afe.wav'}[ident],
                         data, item['rate'], item['channels'])
                if ident == 0:
                    (self.directory / 'tdm_raw.bin').write_bytes(data)
                    for slot, name in enumerate(('mic1', 'reference', 'mic2', 'unused')):
                        mono = bytearray()
                        for pos in range(slot * 2, len(data), 8):
                            mono.extend(data[pos:pos+2])
                        save_wav(self.directory / f'slot{slot}_24k.wav', mono, item['rate'], 1)
            meta = {i: {k: v for k, v in s.items() if k != 'data'} for i, s in self.streams.items()}
            (self.directory / 'timing.json').write_text(json.dumps(meta, indent=2), encoding='utf-8')
            (self.directory / 'stage_timestamps.json').write_text(json.dumps(
                dict(frame_samples=self.frames, blocks=self.timestamps,
                     note='blocks=[stream,byte_offset,bytes,monotonic_us]; AFE/Opus有流水线延迟，不可按数组下标假定同步'), indent=2), encoding='utf-8')
            if set(self.streams) == set(range(7)):
                from verify_audio_stages import verify
                verify(self.directory)
            print(f'完整性校验通过，音频已保存: {self.directory}', flush=True)
            return True
        return False


def main():
    import serial
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='COM7')
    args = parser.parse_args()
    directory = Path(__file__).resolve().parents[3] / 'logs' / time.strftime('audio_probe_%Y%m%d_%H%M%S')
    directory.mkdir(parents=True, exist_ok=False)
    receiver = Receiver(directory)
    # 打开前关闭 DTR/RTS，设备由用户手动复位，避免意外进入下载模式。
    port = serial.Serial(port=None, baudrate=115200, timeout=1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    print('请关闭 IDF 串口监视。连接成功后按一次 P4 RESET。', flush=True)
    with port:
        print('串口已连接，请按 P4 RESET。待机后唤醒小智；第一轮回答结束后直接说现在几点了，自动录音8秒。', flush=True)
        deadline = time.monotonic() + 1800
        with (directory / 'serial.log').open('wb') as log:
            while time.monotonic() < deadline:
                raw = port.readline()
                log.write(raw)
                line = raw.decode('utf-8', errors='replace').strip()
                if any(tag in line for tag in ('AUDIO_PROBE', 'VOICE_STATE:', 'VOICE_STATUS:', '识别结果', '回答文本')):
                    print(line, flush=True)
                if receiver.accept(line):
                    return
    raise SystemExit('等待超时；请检查烧录版本、串口及 logs 下的 serial.log')


if __name__ == '__main__':
    main()
