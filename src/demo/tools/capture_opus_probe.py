"""接收首次续听实际发送的 Opus 包，校验后封装 Ogg 并用 FFmpeg 解码试听。"""
import argparse
import base64
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time
import wave


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def ogg_page(packet, serial, sequence, granule, flags):
    # Ogg 每页只放一个 Opus 包；255 的整数倍须追加 0 长度 lacing 段。
    segments = bytearray([255] * (len(packet) // 255) + [len(packet) % 255])
    page = bytearray(b'OggS')
    page.extend(struct.pack('<BBQIIIB', 0, flags, granule, serial, sequence, 0, len(segments)))
    page.extend(segments)
    page.extend(packet)
    crc = 0
    for byte in page:
        crc ^= byte << 24
        for _ in range(8):
            crc = ((crc << 1) ^ (0x04C11DB7 if crc & 0x80000000 else 0)) & 0xffffffff
    struct.pack_into('<I', page, 22, crc)
    return page


def make_ogg(packets, sample_rate, duration_ms):
    if sample_rate != 16000 or duration_ms != 60 or not packets:
        raise ValueError('Opus 参数或包数无效')
    serial = 0x72102026
    head = struct.pack('<8sBBHIhB', b'OpusHead', 1, 1, 0, sample_rate, 0, 0)
    vendor = b'Lummiss diagnostic'
    tags = b'OpusTags' + struct.pack('<I', len(vendor)) + vendor + struct.pack('<I', 0)
    result = bytearray(ogg_page(head, serial, 0, 0, 2))
    result.extend(ogg_page(tags, serial, 1, 0, 0))
    for index, (_, packet) in enumerate(packets):
        # Ogg Opus granule 单位固定为 48 kHz，与编码器输入采样率无关。
        granule = (index + 1) * duration_ms * 48
        result.extend(ogg_page(packet, serial, index + 2, granule,
                               4 if index + 1 == len(packets) else 0))
    return result


class Receiver:
    def __init__(self, directory, ffmpeg):
        self.directory = directory
        self.ffmpeg = ffmpeg
        self.info = None
        self.data = bytearray()
        self.ended = False

    def accept(self, line):
        match = re.search(r'OP_(BEGIN|DATA|END|DONE)\b(.*)', line)
        if not match:
            return False
        kind, tail = match.groups()
        fields = tail.split()
        if kind == 'BEGIN':
            if self.info is not None:
                raise ValueError('重复导出或设备重启')
            version, rate, duration, count, size, first, last, overflow = map(int, fields)
            if version != 1 or rate != 16000 or duration != 60 or count <= 0 or size <= 0:
                raise ValueError('Opus 导出头无效')
            self.info = dict(sample_rate=rate, frame_duration_ms=duration,
                             packet_count=count, bytes=size, first_us=first,
                             last_us=last, overflow=overflow)
            print(f'接收 {count} 个 Opus 包，共 {size} 字节', flush=True)
        elif kind == 'DATA':
            if self.info is None or self.ended:
                raise ValueError('缺少 OP_BEGIN 或数据已结束')
            offset = int(fields[0])
            if offset != len(self.data):
                raise ValueError(f'串口丢行：应为 {len(self.data)}，实际 {offset}')
            self.data.extend(base64.b64decode(fields[1], validate=True))
            if len(self.data) > self.info['bytes']:
                raise ValueError('接收长度超出声明长度')
        elif kind == 'END':
            if self.info is None or len(self.data) != self.info['bytes'] or \
                    checksum(self.data) != int(fields[0], 16):
                raise ValueError('Opus 长度或校验和不符')
            self.ended = True
        else:
            if not self.ended:
                raise ValueError('缺少 OP_END')
            packets = []
            pos = 0
            while pos < len(self.data):
                if pos + 10 > len(self.data):
                    raise ValueError('Opus 记录头不完整')
                length, timestamp = struct.unpack_from('<HQ', self.data, pos)
                pos += 10
                if length == 0 or length > 1400 or pos + length > len(self.data):
                    raise ValueError('Opus 包长度无效')
                packets.append((timestamp, bytes(self.data[pos:pos + length])))
                pos += length
            if len(packets) != self.info['packet_count'] or self.info['overflow']:
                raise ValueError('Opus 包数不符或采集缓存溢出')
            stamps = [timestamp for timestamp, _ in packets]
            if stamps != sorted(stamps) or len(set(stamps)) != len(stamps):
                raise ValueError('Opus 包时间戳顺序异常')
            (self.directory / 'uplink_sent.opus_packets.bin').write_bytes(self.data)
            ogg_path = self.directory / 'uplink_sent.ogg'
            ogg_path.write_bytes(make_ogg(packets, self.info['sample_rate'],
                                          self.info['frame_duration_ms']))
            wav_path = self.directory / 'uplink_sent_decoded.wav'
            result = subprocess.run(
                [self.ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-xerror',
                 '-i', str(ogg_path), '-ar', '16000', '-ac', '1', '-c:a', 'pcm_s16le',
                 '-y', str(wav_path)], capture_output=True, text=True, timeout=120)
            if result.returncode != 0:
                raise ValueError(f'FFmpeg 连续解码失败：{result.stderr.strip()}')
            with wave.open(str(wav_path), 'rb') as wav:
                decoded_samples = wav.getnframes()
                if (wav.getnchannels(), wav.getframerate(), wav.getsampwidth()) != (1, 16000, 2):
                    raise ValueError('解码后 WAV 格式异常')
            self.info.update(decoded_samples=decoded_samples,
                             packet_bytes_min=min(len(packet) for _, packet in packets),
                             packet_bytes_max=max(len(packet) for _, packet in packets),
                             send_gap_us_max=max((b - a for a, b in zip(stamps, stamps[1:])),
                                                 default=0))
            (self.directory / 'opus_probe.json').write_text(
                json.dumps(self.info, ensure_ascii=False, indent=2), encoding='utf-8')
            print(json.dumps(self.info, ensure_ascii=False, indent=2), flush=True)
            print(f'已解码，可试听：{wav_path}', flush=True)
            return True
        return False


def main():
    import serial
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='COM7')
    args = parser.parse_args()
    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        raise SystemExit('未找到 FFmpeg，请先将 ffmpeg.exe 加入 PATH')
    directory = Path(__file__).resolve().parents[3] / 'logs' / time.strftime('opus_probe_%Y%m%d_%H%M%S')
    directory.mkdir(parents=True, exist_ok=False)
    receiver = Receiver(directory, ffmpeg)
    port = serial.Serial(port=None, baudrate=115200, timeout=1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    print('请先关闭 IDF 串口监视器；连接后按一次 P4 RESET。', flush=True)
    with port, (directory / 'serial.log').open('wb') as log:
        print('唤醒后等待第一轮回答结束，随后清楚说“现在几点了”；等待 OP_DONE，勿再次复位。', flush=True)
        deadline = time.monotonic() + 600
        while time.monotonic() < deadline:
            raw = port.readline()
            log.write(raw)
            line = raw.decode('utf-8', errors='replace').strip()
            if any(tag in line for tag in ('OPUS_PROBE', 'VOICE_STATE:', 'VOICE_STATUS:', '识别结果')):
                print(line, flush=True)
            if receiver.accept(line):
                return
    raise SystemExit(f'等待超时；原始串口日志在 {directory / "serial.log"}')


if __name__ == '__main__':
    main()
