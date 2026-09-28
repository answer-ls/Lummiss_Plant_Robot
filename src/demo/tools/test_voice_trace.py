"""校验实际时序计数函数：首包、监听轮次和迟到STT均不污染当前会话。"""
from pathlib import Path
import subprocess,tempfile,shutil
s=(Path(__file__).resolve().parents[1]/'components/xiaozhi_audio/xiaozhi_audio.c').read_text(encoding='utf-8')
a=s.index('static portMUX_TYPE s_trace_lock');b=s.index('\n#if defined(CONFIG_CLOUD_PROTOCOL_V3)',a)
head='''#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define ESP_OK 0
#define ESP_LOGI(tag,fmt,...) printf(fmt "\\n",__VA_ARGS__)
static int64_t esp_timer_get_time(void) {static int64_t t;return ++t;}
'''
main='''
int main(void) {
voice_trace("WAKE_DETECTED",NULL,0);
voice_trace("SESSION_BOUND","new",0);
voice_trace("PREROLL_PACKET_TX",NULL,0);
voice_trace("LISTEN_START_BEGIN",NULL,0);
voice_trace("STT_RX","old",0);
voice_trace("STT_RX","new",0);
voice_trace("LIVE_OPUS_TX",NULL,0);
voice_trace("LIVE_OPUS_TX",NULL,0);
voice_trace("LISTEN_START_BEGIN",NULL,0);
voice_trace("LIVE_OPUS_TX",NULL,0);
voice_trace("STT_RX","new",0);
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'trace.c').write_text(head+s[a:b]+main,encoding='utf-8')
    subprocess.run([shutil.which('gcc') or r'D:\mingw64\bin\gcc.exe',str(p/'trace.c'),'-o',str(p/'trace.exe')],check=True)
    out=subprocess.check_output([str(p/'trace.exe')],text=True)
assert out.count('event=FIRST_LIVE_OPUS_TX')==2
assert out.count('event=FIRST_STT_RX')==2
assert 'session_id=old' in out and 'event=STT_RX_UNMATCHED' in out
assert 'round=1 event=FIRST_STT_RX ret=0 preroll_packets=1 live_packets=0' in out
assert 'round=2 event=FIRST_STT_RX ret=0 preroll_packets=1 live_packets=1' in out
print('PASS trace session / first live packet / preroll / listening rounds / stale STT')
