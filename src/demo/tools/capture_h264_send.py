"""接收实际提交esp_peer的H264，逐帧校验并用FFmpeg连续解码。"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import time
from capture_jpeg_failure import checksum


class Receiver:
    def __init__(self, directory):
        self.directory = directory
        self.metadata = None
        self.frames = []
        self.data = bytearray()

    def accept(self, line):
        line = line.strip()
        if line.startswith('HP_BEGIN '):
            if self.metadata is not None:
                raise ValueError('重复BEGIN，可能发生复位')
            self.metadata = json.loads(line[9:])
            if not 0 < self.metadata['frames'] <= 192 or not 0 < self.metadata['bytes'] <= 2*1024*1024:
                raise ValueError('导出长度异常')
        elif line.startswith('HP_FRAME '):
            if self.metadata is None:
                raise ValueError('缺少BEGIN')
            f = json.loads(line[9:])
            if f['index'] != len(self.frames) or f['offset'] != len(self.data) or f['size'] <= 0:
                raise ValueError('帧顺序或偏移异常')
            if self.frames and len(self.data) != self.frames[-1]['offset']+self.frames[-1]['size']:
                raise ValueError('上一帧导出不完整')
            self.frames.append(f)
        elif line.startswith('HP_DATA '):
            m = re.fullmatch(r'HP_DATA (\d+) ([0-9a-f]+)', line)
            if not m or not self.frames or int(m[1]) != len(self.data):
                raise ValueError('数据缺块/重复/损坏')
            chunk = bytes.fromhex(m[2])
            if len(self.data)+len(chunk) > self.frames[-1]['offset']+self.frames[-1]['size']:
                raise ValueError('数据越过帧边界')
            self.data.extend(chunk)
        elif line == 'HP_DONE':
            if not self.metadata or len(self.frames) != self.metadata['frames'] or len(self.data) != self.metadata['bytes']:
                raise ValueError('导出不完整')
            for f in self.frames:
                frame = self.data[f['offset']:f['offset']+f['size']]
                if checksum(frame) != f['hash']:
                    raise ValueError(f'帧{f["index"]}校验失败')
                f['sha256'] = hashlib.sha256(frame).hexdigest()
                # Annex-B的起始码不属于NAL内容，记录实际NAL类型以核对IDR/SPS/PPS。
                f['nal_types'] = [self.data[f['offset']+m.end()] & 31
                                  for m in re.finditer(b'\x00\x00(?:\x00)?\x01', frame)
                                  if m.end() < len(frame)]
            (self.directory/'submitted.h264').write_bytes(self.data)
            (self.directory/'capture.json').write_text(json.dumps(
                dict(capture=self.metadata, frames=self.frames), indent=2), encoding='utf-8')
            return True
        return False


def analyze(directory):
    meta = json.loads((directory/'capture.json').read_text(encoding='utf-8'))
    frames = meta['frames']
    data = (directory/'submitted.h264').read_bytes()
    if len(data) != meta['capture']['bytes']:
        raise ValueError('文件长度异常')
    for f in frames:
        if checksum(data[f['offset']:f['offset']+f['size']]) != f['hash']:
            raise ValueError('文件校验失败')
    result = dict(submitted_frames=len(frames),
                  send_failures=sum(f['ret'] != 0 for f in frames),
                  pts_non_increasing=sum(b['pts_ms'] <= a['pts_ms'] for a, b in zip(frames, frames[1:])),
                  first_nal_types=frames[0]['nal_types'],
                  unique_encoded_frames=len({f['sha256'] for f in frames}))
    ffmpeg = shutil.which('ffmpeg')
    if ffmpeg:
        proc = subprocess.run([ffmpeg, '-hide_banner', '-v', 'warning', '-xerror',
                               '-err_detect', 'explode', '-f', 'h264', '-i', str(directory/'submitted.h264'),
                               '-map', '0:v:0', '-f', 'framemd5', '-'], capture_output=True)
        (directory/'decode_stderr.log').write_bytes(proc.stderr)
        (directory/'decoded.framemd5').write_bytes(proc.stdout)
        rows = [line.split(',') for line in proc.stdout.decode(errors='replace').splitlines()
                if line and not line.startswith('#')]
        hashes = [row[-1].strip() for row in rows]
        result.update(decoder='FFmpeg', decode_returncode=proc.returncode,
                      decoded_frames=len(hashes), unique_decoded_frames=len(set(hashes)),
                      decode_complete=proc.returncode == 0 and len(hashes) == len(frames),
                      consecutive_pixel_changes=sum(a != b for a, b in zip(hashes, hashes[1:])))
        duration = meta['capture']['duration_us']/1e6
        fps = (len(frames)-1)/duration if duration > 0 else 20
        if proc.returncode == 0 and hashes:
            # 预览用平均提交帧率；真实逐帧PTS完整保存在capture.json，不用MP4推断RTP时序。
            preview = subprocess.run([ffmpeg, '-y', '-hide_banner', '-v', 'warning', '-r', str(fps),
                '-f', 'h264', '-i', str(directory/'submitted.h264'), '-an', '-c:v', 'libx264',
                '-preset', 'fast', '-crf', '18', '-pix_fmt', 'yuv420p', str(directory/'preview.mp4')], capture_output=True)
            (directory/'preview_stderr.log').write_bytes(preview.stderr)
            result['preview_ok'] = preview.returncode == 0
            result['preview_fps'] = fps
    else:
        result['decoder_error'] = '未找到ffmpeg；码流已保存，可稍后使用--analyze'
    result['note'] = '像素hash变化证明解码帧不同，不保证场景运动正确；请同时观看preview.mp4。MP4使用平均帧率，不代表实际RTP时间戳。'
    (directory/'analysis.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(result, ensure_ascii=False, indent=2))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM7')
    parser.add_argument('--analyze', type=Path)
    args = parser.parse_args()
    if args.analyze:
        analyze(args.analyze); return
    import serial
    directory = Path(__file__).resolve().parents[3]/'logs'/time.strftime('h264_send_%Y%m%d_%H%M%S')
    directory.mkdir(parents=True, exist_ok=False)
    receiver = Receiver(directory)
    port = serial.Serial(port=None, baudrate=115200, timeout=1)
    port.dtr = port.rts = False
    port.port = args.port
    print(f'保存目录：{directory}\n关闭IDF串口监视；脚本连接后按P4 RESET。', flush=True)
    with port, (directory/'serial.log').open('wb') as log:
        print('开启预览并持续移动手掌5秒以上。出现H264_PROBE CAPTURED后停止预览；等待HP_DONE，勿复位或重开预览。', flush=True)
        deadline = time.monotonic()+1800
        while time.monotonic() < deadline:
            raw = port.readline(); log.write(raw)
            line = raw.decode('utf-8', errors='replace').strip()
            if 'H264_PROBE' in line or line.startswith(('HP_BEGIN ', 'HP_DONE')):
                print(line, flush=True)
            if receiver.accept(line):
                log.flush(); analyze(directory)
                print(f'采集完成：{directory}'); return
    raise SystemExit('采集超时，serial.log已保留')


if __name__ == '__main__':
    main()
