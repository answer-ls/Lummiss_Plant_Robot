"""运行实际 AFE 输出函数，证明静音 PCM 不丢弃、旧会话 PCM 不泄漏。"""
from pathlib import Path
import shutil
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] /
          "components/xiaozhi_audio/wake_word.c").read_text(encoding="utf-8")
start = source.index("static void handle_voice_result(")
end = source.index("\n}\n", start) + 2
function = source[start:end]
test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define VAD_SPEECH 1
#define portMAX_DELAY 0
typedef struct {int vad_state, data_size; int16_t *data;} afe_fetch_result_t;
typedef struct {
    uint32_t speech_frames,silence_frames,speech_begin_count,speech_end_count;
    uint32_t output_samples,output_drop,output_generation;
    bool voice_detected,uplink_enabled;
    int output_lock,output_stream;
} wake_word_ctx_t;
static uint8_t output[4096];
static size_t used;
static void stats_inc(volatile uint32_t *v) {++*v;}
static void xSemaphoreTake(int x,int y) {(void)x;(void)y;}
static void xSemaphoreGive(int x) {(void)x;}
static size_t xStreamBufferSpacesAvailable(int x) {(void)x; return sizeof(output)-used;}
static void xStreamBufferSend(int x,const void *p,size_t n,int t) {
    (void)x;(void)t; memcpy(output+used,p,n);used+=n;
}
FUNCTION
int main(void) {
    wake_word_ctx_t ctx={0};
    int16_t pcm[512]; for(int i=0;i<512;i++) pcm[i]=i*17;
    afe_fetch_result_t frame={.data=pcm,.data_size=sizeof(pcm),.vad_state=0};
    handle_voice_result(&ctx,&frame,0);
    assert(used==0 && ctx.silence_frames==1); /* 待机仍有真实 VAD，禁止上传。 */
    ctx.uplink_enabled=true; ctx.output_generation=1;
    handle_voice_result(&ctx,&frame,1);
    assert(used==1024 && !memcmp(output,pcm,sizeof(pcm))); /* 静音全量输出。 */
    frame.vad_state=VAD_SPEECH;
    handle_voice_result(&ctx,&frame,1);
    handle_voice_result(&ctx,&frame,1);
    assert(ctx.speech_begin_count==1 && ctx.speech_frames==2 && ctx.voice_detected);
    frame.vad_state=0;
    handle_voice_result(&ctx,&frame,1);
    assert(ctx.speech_end_count==1 && !ctx.voice_detected && used==4096);
    assert(ctx.output_samples==2048 && ctx.output_drop==0);
    used=0; ctx.uplink_enabled=false;
    handle_voice_result(&ctx,&frame,1); assert(used==0);
    ctx.uplink_enabled=true; ctx.output_generation=2;
    handle_voice_result(&ctx,&frame,1); assert(used==0); /* fetch 跨会话不能补入旧帧。 */
    handle_voice_result(&ctx,&frame,2); assert(used==1024);
    puts("PASS silence PCM preserved / VAD edges / processor gate / stale fetch rejected");
}
'''
gcc = shutil.which("gcc")
if not gcc:
    raise SystemExit("需要主机 gcc")
with tempfile.TemporaryDirectory(prefix="lummiss-afe-output-") as folder:
    cfile = Path(folder) / "output.c"
    exe = Path(folder) / "output.exe"
    cfile.write_text(test.replace("FUNCTION", function), encoding="utf-8")
    subprocess.run([gcc, "-std=c11", "-Wall", "-Werror", str(cfile), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
