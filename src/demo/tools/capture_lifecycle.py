"""接收生命周期等长raw窗口和寄存器快照；不控制串口复位。"""
import argparse,base64,json,re,time
from pathlib import Path
from capture_audio_probe import checksum,save_wav

class Receiver:
    def __init__(self,folder):
        self.folder=Path(folder);self.items={};self.events=[]
    def accept(self,line):
        m=re.search(r'LC_(BEGIN|DATA|END|DONE|REG|SNAPSHOT|EVENT)\b(.*)',line)
        if not m:return False
        kind,tail=m.groups();f=tail.split()
        if kind=='EVENT':self.events.append(dict(us=int(f[0]),state=f[1]));return False
        if kind=='DONE':
            if set(self.items)!=set(range(9)) or not all(x['done'] for x in self.items.values()):raise ValueError('导出缺组')
            meta={i:{k:v for k,v in item.items() if k!='data'} for i,item in self.items.items()}
            (self.folder/'lifecycle.json').write_text(json.dumps(dict(records=meta,events=self.events,pa_muted='UNSUPPORTED_NO_PA_GPIO'),indent=2),encoding='utf-8')
            return True
        ident=int(f[0])
        if kind=='BEGIN':
            if ident in self.items:raise ValueError('设备重启/重复组')
            name,size,mark,first,last,crossed,error=f[1:]
            if int(size)>192000 or int(size)%8:raise ValueError('数据长度不合法')
            self.items[ident]=dict(name=name,size=int(size),mark_us=int(mark),first_us=int(first),last_us=int(last),crossed=int(crossed),error=int(error),snapshots={},data=bytearray(),done=False)
        elif kind=='SNAPSHOT':
            self.items[ident]['snapshots'][f[1]]=dict(us=int(f[2]),hw=[int(x,16) for x in f[3:]],regs={})
        elif kind=='REG':self.items[ident]['snapshots'][f[1]]['regs'][f[2]]=int(f[3])
        elif kind=='DATA':
            item=self.items[ident]
            if int(f[1])!=len(item['data']):raise ValueError('串口丢行')
            item['data'].extend(base64.b64decode(f[2],validate=True))
            if len(item['data'])>item['size']:raise ValueError('数据超过声明长度')
        elif kind=='END':
            item=self.items[ident];data=item['data']
            if item['done'] or len(data)!=item['size'] or checksum(data)!=int(f[1],16):raise ValueError('PCM长度/校验失败')
            item['done']=True;folder=self.folder/item['name'];folder.mkdir()
            (folder/'tdm_raw.bin').write_bytes(data);save_wav(folder/'raw_4slot_24k.wav',data,24000,4)
            for slot in range(4):
                mono=bytearray(len(data)//4);mono[0::2]=data[slot*2::8];mono[1::2]=data[slot*2+1::8]
                save_wav(folder/f'slot{slot}.wav',mono,24000,1)
            print(f"{item['name']} bytes={len(data)} crossed={item['crossed']} error={item['error']}",flush=True)
        return False

def main():
    import serial
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--port',default='COM7');args=parser.parse_args()
    folder=Path(__file__).resolve().parents[3]/'logs'/time.strftime('lifecycle_%Y%m%d_%H%M%S');folder.mkdir()
    receiver=Receiver(folder);port=serial.Serial();port.port=args.port;port.baudrate=115200;port.timeout=1;port.dtr=port.rts=False;port.open()
    print(f'{folder}\n按P4 RESET；启动会短暂播放测试音。待机后唤醒并对话，回答后继续说话。',flush=True)
    deadline=time.monotonic()+900
    with port,(folder/'serial.log').open('w',encoding='utf-8') as log:
        while time.monotonic()<deadline:
            line=port.readline().decode('utf-8',errors='replace');log.write(line)
            if any(tag in line for tag in ('LIFE:','VOICE_STATE:','VOICE_TRACE:','VAD_EDGE:','VAD_DIAG:','LC_BEGIN','LC_DONE')):print(line.rstrip(),flush=True);log.flush()
            if receiver.accept(line):print('采集完成；短读、跨状态、未出现状态均保留原样，不补零。',flush=True);return
    raise TimeoutError('等待超时，日志已保留')
if __name__=='__main__':main()
