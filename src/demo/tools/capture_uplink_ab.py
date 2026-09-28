"""采集同一固件两次连续监听的UDP A/B诊断；不补齐丢失数据。"""
import argparse
import base64
import csv
import json
from datetime import datetime
from pathlib import Path
import wave


def checksum(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def save_audio(folder, data):
    if len(data) % 8:
        raise ValueError('四槽帧长度错误')
    (folder / 'tdm_raw.bin').write_bytes(data)
    for slot in [-1, 0, 1, 2, 3]:
        pcm = data if slot == -1 else b''.join(data[i+slot*2:i+slot*2+2] for i in range(0, len(data), 8))
        name = 'raw_4slot_24k.wav' if slot == -1 else f'slot{slot}_24k.wav'
        with wave.open(str(folder / name), 'wb') as out:
            out.setparams((4 if slot == -1 else 1, 2, 24000, 0, 'NONE', 'not compressed'))
            out.writeframes(pcm)


def spectrum(data):
    # numpy可选；未安装时仍完整保存录音和时序，不能假称完成频谱比较。
    try:
        import numpy as np
    except ImportError:
        return {'error': '未安装numpy，频谱未计算'}
    x = np.frombuffer(data, dtype='<i2').reshape(-1, 4).astype(float)
    result = {}
    for slot in range(4):
        y = x[:, slot]
        if len(y) < 2:
            continue
        dc = float(y.mean())
        f = np.fft.rfftfreq(len(y), 1/24000)
        power = abs(np.fft.rfft((y-dc)*np.hanning(len(y))))**2
        total = float(power.sum())
        result[f'slot{slot}'] = {
            'rms': float(np.sqrt(np.mean(y*y))), 'dc': dc,
            'peak_hz': float(f[int(power.argmax())]),
            'below_300_hz_fraction': float(power[f < 300].sum()/total) if total else 0,
            'peak_bins': [{'hz': float(f[k]), 'power': float(power[k])}
                          for k in np.argsort(power)[-8:][::-1]],
        }
    return result


class Capture:
    def __init__(self, root):
        self.root, self.phases = root, {}

    def line(self, line):
        pos = line.find('CD_')
        if pos < 0:
            return False
        parts = line[pos:].split()
        kind = parts[0]
        if kind == 'CD_DONE':
            if len(self.phases) != 2 or not all(p.get('complete') for p in self.phases.values()):
                raise ValueError('A/B导出不完整')
            return True
        if kind not in ('CD_BEGIN', 'CD_ROW', 'CD_STAT', 'CD_GAPS', 'CD_DATA', 'CD_END'):
            return False
        phase = int(parts[1])
        if kind == 'CD_BEGIN':
            keys = ['raw_bytes','start_us','finish_us','crossed_or_send_error','truncated','read_errors','rx_overflow_count','packet_count','bytes_count']
            if len(parts[2:]) != len(keys):
                raise ValueError('诊断头格式不匹配')
            self.phases[phase] = dict(zip(keys, map(int, parts[2:])), raw=bytearray(), rows=[], stats={})
        else:
            p = self.phases[phase]
            if kind == 'CD_ROW':
                p['rows'].append(list(map(int, parts[2:])))
            elif kind == 'CD_STAT':
                p['stats'][parts[2]] = dict(zip(['count','max','avg','p95','p99'],map(float,parts[3:])))
            elif kind == 'CD_GAPS':
                p['gaps'] = dict(zip(['gt15ms','gt30ms','gt50ms','gt60ms','gt100ms'],map(int,parts[2:])))
            elif kind == 'CD_DATA':
                if int(parts[2]) != len(p['raw']):
                    raise ValueError('串口块丢失或重复')
                p['raw'].extend(base64.b64decode(parts[3], validate=True))
            elif kind == 'CD_END':
                if len(p['raw']) != p['raw_bytes'] or checksum(p['raw']) != int(parts[2],16):
                    raise ValueError('录音长度/hash校验失败')
                folder = self.root / ('A_UDP' if phase == 0 else 'B_LOCAL')
                folder.mkdir(parents=True, exist_ok=True)
                save_audio(folder, p['raw'])
                p['spectrum'] = spectrum(p['raw'])
                # 窗口状态、传输和数据完整性必须先通过，声音是否相同仍由操作者确认。
                p['valid'] = not any(p[k] for k in ['crossed_or_send_error','truncated','read_errors']) and p['packet_count'] > 0 and p['raw_bytes'] > 0
                p['complete'] = True
                with (folder/'reads.csv').open('w',newline='',encoding='utf-8') as out:
                    writer=csv.writer(out)
                    writer.writerow(['read_begin_us','read_end_us','next_read_begin_us','ret'])
                    writer.writerows(p['rows'])
                summary={k:v for k,v in p.items() if k not in ('raw','rows')}
                (folder/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
                print(folder, '有效窗口' if p['valid'] else '无效对照，需重新采集')
        return False


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--port',default='COM7')
    parser.add_argument('--analyze',type=Path,help='离线重算指定采集目录的频谱，需要numpy')
    args=parser.parse_args()
    if args.analyze:
        for name in ['A_UDP','B_LOCAL']:
            folder=args.analyze/name
            summary=json.loads((folder/'summary.json').read_text(encoding='utf-8'))
            summary['spectrum']=spectrum((folder/'tdm_raw.bin').read_bytes())
            (folder/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
            print(name,summary['spectrum'])
        return
    import serial
    root=Path(__file__).resolve().parents[3]/'logs'/('uplink_ab_'+datetime.now().strftime('%Y%m%d_%H%M%S'))
    root.mkdir(parents=True)
    capture=Capture(root)
    print(root)
    print('按P4 RESET。进行两次唤醒/对话：每次回答结束后播放相同固定声源至少5秒。A正常UDP，B仅本地计数。')
    print('若4秒内状态改变，该窗口作废。两段录完后自动导出，请等待CD_DONE，勿再次复位。')
    port=serial.Serial()
    port.port=args.port;port.baudrate=115200;port.timeout=1;port.dtr=False;port.rts=False
    port.open()
    with port, (root/'serial.log').open('w',encoding='utf-8') as log:
        while True:
            line=port.readline().decode('utf-8',errors='replace')
            if not line:
                continue
            log.write(line);log.flush()
            if 'CD_DATA' not in line and ('CD_' in line or 'VOICE_STATE' in line or 'CAP_DIAG' in line):
                print(line.strip())
            if capture.line(line):
                print('采集完成；只有时序/overflow和raw同时改善才支持同步网络阻塞假设。')
                break


if __name__ == '__main__':
    main()
