"""独立ES7210测试接收器：原始字节校验、四槽WAV、逐槽统计，不滤波不重采样。"""
import argparse
import array
import base64
import hashlib
import json
import math
from pathlib import Path
import sys
import time
import wave
from datetime import datetime


def fnv(data, value=2166136261):
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def statistics(values):
    n = len(values)
    dc = sum(values) / n if n else 0
    power = sum(v * v for v in values) / n if n else 0
    return dict(samples=n, dc=dc, rms=math.sqrt(power),
                ac_rms=math.sqrt(max(0, power - dc * dc)),
                peak=max(map(abs, values), default=0),
                clipping=sum(v in (-32768, 32767) for v in values),
                zero_count=values.count(0))


def wav(path, data, channels, rate):
    with wave.open(str(path), 'wb') as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(data)


def save_record(folder, meta, data):
    if len(data) != meta['bytes'] or len(data) % 8:
        raise ValueError('PCM长度错误，不补零或截断')
    folder.mkdir(parents=True, exist_ok=True)
    (folder / 'tdm_raw.bin').write_bytes(data)
    wav(folder / 'raw_4slot.wav', data, 4, meta['rate'])
    pcm = array.array('h', data)
    if sys.byteorder != 'little':
        pcm.byteswap()
    result = dict(meta, sha256=hashlib.sha256(data).hexdigest(), slots={})
    for ch in range(4):
        values = pcm[ch::4]
        result['slots'][str(ch)] = statistics(values)
        if sys.byteorder != 'little':
            values.byteswap()
        wav(folder / f'slot{ch}.wav', values.tobytes(), 1, meta['rate'])
    result['duration_s'] = len(data) / (8 * meta['rate'])
    (folder / 'summary.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    return result


def main():
    import serial
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--groups', nargs='+', type=int, choices=(1, 2, 3), default=[1, 2, 3])
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    out = root / 'logs' / ('hardware_audio_' + datetime.now().strftime('%Y%m%d_%H%M%S'))
    out.mkdir(parents=True, exist_ok=False)
    names = {1: 'MIC1', 2: 'MIC2', 3: 'DUAL'}
    print(f'保存目录：{out}\n关闭IDF串口监视。连接后按P4 RESET，等待HA_READY。')
    # 不自动复位；由用户确认已经烧录独立工程，避免扰动主程序。
    port = serial.Serial(port=None, baudrate=args.baud, timeout=1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    with port, (out / 'serial.log').open('wb') as log:
        index = 0
        meta = None
        data = bytearray()
        checksum = 2166136261
        last = time.monotonic()
        while index < len(args.groups):
            line = port.readline()
            if not line:
                if time.monotonic() - last > 120:
                    raise TimeoutError('120秒没有串口输出；检查端口、复位和固件，已收到日志保留')
                continue
            last = time.monotonic()
            log.write(line)
            text = line.decode('utf-8', errors='replace').strip()
            if text.startswith('HA_READY'):
                group = args.groups[index]
                input(f'{names[group]}：准备好相同固定声源，按回车开始10秒录音；导出期间勿复位。')
                port.write(f'{group}\n'.encode())
                last = time.monotonic()
            elif text.startswith('HA_BEGIN '):
                if meta is not None:
                    raise ValueError('上一段未结束，拒绝混合PCM')
                meta = json.loads(text[len('HA_BEGIN '):])
                if meta['group'] != args.groups[index]:
                    raise ValueError('测试组不匹配')
                data = bytearray()
                checksum = 2166136261
                print('录音结束，正在导出完整PCM（115200波特率约3分钟）…')
            elif text.startswith('HA_DATA '):
                if meta is None:
                    raise ValueError('缺少HA_BEGIN')
                _, offset, payload = text.split()
                if int(offset) != len(data):
                    raise ValueError(f'串口数据缺块/重块：offset={offset}，expected={len(data)}')
                block = base64.b64decode(payload, validate=True)
                data.extend(block)
                checksum = fnv(block, checksum)
                if len(data) > meta['bytes']:
                    raise ValueError('PCM超过声明长度')
            elif text.startswith('HA_END '):
                if meta is None or checksum != int(text.split()[1], 16):
                    raise ValueError('PCM校验失败，不能当作有效录音')
                result = save_record(out / f'{index+1}_{names[meta["group"]]}', meta, data)
                print(json.dumps(result, ensure_ascii=False, indent=2))
                if not meta['valid'] or result['duration_s'] < 10:
                    print('本组无效：短读/溢出/不足10秒；保存原样供检查，不据此判断麦克风好坏。')
                meta = None
                index += 1
            else:
                print(text)
        print(f'HA_DONE：{out}\n试听slot0.wav和slot2.wav；请提供各组summary.json。')


if __name__ == '__main__':
    main()
