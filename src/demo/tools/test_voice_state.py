"""编译实际状态处理函数，验证门控、迟到消息、播放排空和失败回退。"""
from pathlib import Path
import shutil
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] /
          "components/xiaozhi_audio/xiaozhi_audio.c").read_text(encoding="utf-8")


def function(signature):
    start = source.index(signature + "\n{")
    end = source.index("\n}", start) + 2
    return source[start:end]


harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_CLOUD_PROTOCOL_V3 1
#define ESP_OK 0
static void voice_trace(const char *e,const char *s,int r) {(void)e;(void)s;(void)r;}
static void lifecycle_mark(const char *name) {(void)name;}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define portMAX_DELAY 0
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define XIAOZHI_EVENT_CHANNEL_ACTIVE 4
#define XIAOZHI_EVENT_UPLINK_ENABLED 8
#define XIAOZHI_EVENT_SPEAKING 16
#define XIAOZHI_EVENT_WAKE_ACTIVE 32
#define AMBIENT_LED_STATE_LISTENING 1
#define AMBIENT_LED_STATE_SPEAKING 2
#define AMBIENT_LED_STATE_WAKE_IDLE 0
typedef unsigned EventBits_t;
typedef enum {VOICE_STATE_WAKE_IDLE, VOICE_STATE_CONNECTING, VOICE_STATE_LISTENING,
              VOICE_STATE_SPEAKING, VOICE_STATE_CONTINUOUS_LISTENING} voice_state_t;
typedef enum {VOICE_WAKE, VOICE_TTS_START, VOICE_TTS_STOP, VOICE_END, VOICE_STT} voice_event_type_t;
typedef struct {voice_event_type_t type; char session[80];} voice_event_t;
static const char *voice_state_name(voice_state_t s) {(void)s;return "TEST";}
static voice_state_t s_voice_state;
static int s_events, s_capture_guard, s_playback_queue = 1;
static unsigned bits, resets, stops, wake_requests;
static bool processor, wake, s_wake_word_ready = true, s_rtc_suspended, s_tts_finished;
static bool empty = true, vad;
static int send_result;
static uint32_t s_voice_state_entered_ms, s_last_stt_ms;
static int64_t clock_us = 1000000, s_tts_stop_us;
static char s_session_id[80];
static struct {int64_t last_output_us, last_rx_us;} s_playback_stats;
static unsigned xEventGroupGetBits(int x) {(void)x; return bits;}
static void xEventGroupSetBits(int x,unsigned b) {(void)x; bits |= b;}
static void xEventGroupClearBits(int x,unsigned b) {(void)x; bits &= ~b;}
static void xSemaphoreTake(int x,int y) {(void)x;(void)y;}
static void xSemaphoreGive(int x) {(void)x;}
static void wake_word_enable_voice_processing(bool b) {processor=b;}
static void wake_word_start(void) {wake=true;}
static void wake_word_stop(void) {wake=false;}
static int64_t esp_timer_get_time(void) {return clock_us;}
static void ambient_led_set_state(int x) {(void)x;}
static void cloud_udp_stop(void) {stops++;}
static void xQueueReset(int x) {(void)x; resets++;}
static bool playback_pipeline_empty(void) {return empty;}
static int send_agent_text(const char *text) {(void)text; return send_result;}
static void reset_speech_timing_stats(void) {}
static const char *wake_word_get_last(void) {return "wake";}
static void process_detected_wake_word(const char *w) {
    (void)w;
    if (s_voice_state == VOICE_STATE_WAKE_IDLE) wake_requests++;
}
static void enter_wake_idle(void);
FUNCTIONS
static void event(voice_event_type_t type, const char *sid) {
    voice_event_t e={.type=type};
    snprintf(e.session,sizeof(e.session),"%s",sid);
    handle_voice_event(&e);
}
static void ready(void) {
    strcpy(s_session_id,"new"); bits |= XIAOZHI_EVENT_CHANNEL_ACTIVE;
}
static void gates(bool w,bool p,bool u) {
    assert(wake==w && processor==p);
    assert(!!(bits & XIAOZHI_EVENT_UPLINK_ENABLED)==u);
}
int main(void) {
    set_voice_state(VOICE_STATE_WAKE_IDLE); gates(1,0,0);
    set_voice_state(VOICE_STATE_CONNECTING); gates(0,0,0);
    set_voice_state(VOICE_STATE_LISTENING); gates(0,0,0);
    assert(s_voice_state==VOICE_STATE_CONNECTING); /* Hello 前不能上传。 */
    ready(); set_voice_state(VOICE_STATE_LISTENING); gates(0,1,1);
    vad=false; gates(0,1,1); vad=true; gates(0,1,1);
    event(VOICE_TTS_START,"old"); gates(0,1,1); /* 迟到会话不能改变当前门控。 */
    event(VOICE_TTS_START,"new"); gates(0,0,0);
    resume_listening_after_playback(); gates(0,0,0);
    event(VOICE_TTS_STOP,"new"); resume_listening_after_playback(); gates(0,0,0);
    clock_us+=200000; empty=false; resume_listening_after_playback(); gates(0,0,0);
    empty=true; resume_listening_after_playback(); gates(0,1,1);
    assert(s_voice_state==VOICE_STATE_CONTINUOUS_LISTENING);
    event(VOICE_END,"old"); gates(0,1,1);
    event(VOICE_END,"new"); gates(1,0,0); assert(!s_session_id[0] && stops==1);
    event(VOICE_TTS_START,"new"); gates(1,0,0);
    ready(); set_voice_state(VOICE_STATE_SPEAKING);
    event(VOICE_TTS_STOP,"new"); clock_us+=200000; send_result=-1;
    resume_listening_after_playback(); gates(1,0,0); assert(!s_session_id[0]);
    s_rtc_suspended=true; enter_wake_idle(); gates(0,0,0);
    event(VOICE_WAKE,""); assert(wake_requests==0);
    s_rtc_suspended=false; set_voice_state(VOICE_STATE_WAKE_IDLE); gates(1,0,0);
    event(VOICE_WAKE,""); assert(wake_requests==1);
    puts("PASS voice gates / hello readiness / stale sessions / TTS drain / failure cleanup / RTC gates");
}
'''
functions = "\n".join(function(name) for name in [
    "static void set_voice_state(voice_state_t next)",
    "static void enter_wake_idle(void)",
    "static void resume_listening_after_playback(void)",
    "static void handle_voice_event(const voice_event_t *incoming)"])
gcc = shutil.which("gcc")
if not gcc:
    raise SystemExit("需要主机 gcc")
with tempfile.TemporaryDirectory(prefix="lummiss-voice-test-") as folder:
    cfile = Path(folder) / "voice.c"
    exe = Path(folder) / "voice.exe"
    cfile.write_text(harness.replace("FUNCTIONS", functions), encoding="utf-8")
    subprocess.run([gcc, "-std=c11", "-Wall", "-Wno-unused-variable", str(cfile), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
