"""接收首次硬解失败帧，严格校验导出完整性，并用 PC libjpeg 解码。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import time
import warnings


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def analyze(directory):
    from PIL import Image, ImageFile
    ImageFile.LOAD_TRUNCATED_IMAGES = False
    metadata = json.loads((directory / 'capture.json').read_text(encoding='utf-8'))
    contents = [(directory / f'{name}.jpg').read_bytes() for name in ('before', 'after')]
    result = {'capture': metadata, 'pc_decoder': 'Pillow/libjpeg', 'files': {}}
    for name, data in zip(('before', 'after'), contents):
        if len(data) != metadata['size'] or checksum(data) != metadata[f'{name}_hash']:
            raise ValueError(f'{name}: 文件长度或传输校验失败，不可用于结论')
        entry = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        try:
            with warnings.catch_warnings(record=True) as caught:
                warnings.simplefilter('always')
                with Image.open(directory / f'{name}.jpg') as picture:
                    picture.load()
                    entry.update(decode_ok=True, size=list(picture.size), mode=picture.mode)
                    picture.save(directory / f'{name}.png')
                entry['warnings'] = [str(item.message) for item in caught]
        except Exception as exc:
            entry.update(decode_ok=False, error=str(exc))
        result['files'][name] = entry
    mismatch = sum(a != b for a, b in zip(*contents))
    first = next((i for i, (a, b) in enumerate(zip(*contents)) if a != b), -1)
    if mismatch != metadata['mismatch'] or first != metadata['first_diff']:
        raise ValueError('PC逐字节比较与固件统计不一致')
    result.update(byte_identical=mismatch == 0, mismatch=mismatch, first_diff=first)
    result['note'] = 'PC解码成功不代表码流严格合法；libjpeg可能容忍硬件不接受的错误。CPU副本相同也不能单独排除DMA缓存可见性问题。'
    (directory / 'analysis.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False, indent=2))


class Receiver:
    def __init__(self, directory):
        self.directory = directory
        self.metadata = None
        self.data = [bytearray(), bytearray()]

    def accept(self, line):
        # 协议必须独占整行，提示文字中的“等待JP_DONE”不是结束标记。
        line = line.strip()
        match = re.fullmatch(r'JP_BEGIN (\{.*\})', line)
        if match:
            if self.metadata is not None:
                raise ValueError('重复BEGIN，可能发生了复位；请重新采集')
            self.metadata = json.loads(match[1])
            if not 0 < self.metadata['size'] <= 4 * 1024 * 1024:
                raise ValueError('异常帧长度')
            return False
        match = re.fullmatch(r'JP_DATA ([01]) (\d+) ([0-9a-f]+)', line)
        if match:
            if self.metadata is None:
                raise ValueError('缺少BEGIN；请从设备复位前开始接收')
            index, offset = int(match[1]), int(match[2])
            if offset != len(self.data[index]):
                raise ValueError('导出缺块或重复块，保留serial.log但不生成伪完整JPEG')
            self.data[index].extend(bytes.fromhex(match[3]))
            return False
        if line != 'JP_DONE':
            return False
        if self.metadata is None:
            raise ValueError('缺少BEGIN')
        for name, data in zip(('before', 'after'), self.data):
            if len(data) != self.metadata['size'] or checksum(data) != self.metadata[f'{name}_hash']:
                raise ValueError(f'{name}: 导出不完整或校验失败')
        for name, data in zip(('before', 'after'), self.data):
            (self.directory / f'{name}.jpg').write_bytes(data)
        (self.directory / 'capture.json').write_text(json.dumps(self.metadata, indent=2), encoding='utf-8')
        return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM7')
    parser.add_argument('--analyze', type=Path, help='对已有导出目录重新解码')
    args = parser.parse_args()
    if args.analyze:
        analyze(args.analyze)
        return
    import serial
    directory = Path(__file__).resolve().parents[3] / 'logs' / time.strftime('jpeg_failure_%Y%m%d_%H%M%S')
    directory.mkdir(parents=True, exist_ok=False)
    receiver = Receiver(directory)
    port = serial.Serial(port=None, baudrate=115200, timeout=1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    print(f'保存目录：{directory}\n请关闭IDF串口监视，连接后按P4 RESET。', flush=True)
    with port, (directory / 'serial.log').open('wb') as log:
        print('打开RTC预览；出现CAPTURED后停止预览，保持串口连接，等待JP_DONE。不要复位或再次开启预览。', flush=True)
        deadline = time.monotonic() + 1800
        while time.monotonic() < deadline:
            raw = port.readline()
            log.write(raw)
            line = raw.decode('utf-8', errors='replace')
            if 'JPEG_PROBE' in line or 'JPEG_TRACE' in line or 'JP_BEGIN' in line or 'JP_DONE' in line:
                print(line.strip(), flush=True)
            if receiver.accept(line):
                log.flush()
                try:
                    analyze(directory)
                except ImportError:
                    print('JPEG已保存且校验通过；当前Python缺少Pillow，安装后用--analyze再次分析。')
                print(f'采集完成：{directory}')
                return
    raise SystemExit('等待超时；原始串口日志已保留。')


if __name__ == '__main__':
    main()
