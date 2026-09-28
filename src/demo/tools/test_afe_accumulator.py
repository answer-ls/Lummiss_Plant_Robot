"""编译当前实际 wake_word_feed，验证多次 feed 和跨批次余量不丢样。"""
from pathlib import Path
import shutil
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] /
          "components/xiaozhi_audio/wake_word.c").read_text(encoding="utf-8")
function = source[source.index("void wake_word_feed("):source.index("void wake_word_enable_voice_processing(")]
test = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define WAKE_EVENT_FEED_STARTED 2
static void xEventGroupSetBits(void *group, int bits) {(void)group; (void)bits;}
static void stats_inc(volatile uint32_t *v) {++*v;}
struct iface {int (*feed)(void *, const int16_t *);};
static struct {
    void *afe_data, *event_group;
    int16_t *feed_buffer;
    size_t feed_chunk_size, feed_buffer_count;
    int channels;
    struct iface *afe_iface;
    volatile uint32_t feed_count, feed_fail, feed_samples, accumulator_remaining_samples;
} s_ww;
static size_t consumed;
static int16_t sample(size_t i) {return (int16_t)((i * 37) % 60001 - 30000);}
static int mock_feed(void *ctx, const int16_t *data) {
    (void)ctx;
    for (size_t i = 0; i < s_ww.feed_chunk_size; ++i)
        assert(data[i] == sample(consumed++));
    return s_ww.feed_chunk_size;
}
FUNCTION
int main(void) {
    const int chunks[] = {512, 1024};
    const int batches[] = {160, 960, 2048, 4096};
    struct iface api = {mock_feed};
    int16_t accumulator[1024 * 3], input[4096 * 3];
    for (int c = 0; c < 2; ++c) for (int b = 0; b < 4; ++b) {
        memset(&s_ww, 0, sizeof(s_ww));
        s_ww.channels = 3;
        s_ww.afe_data = s_ww.event_group = &s_ww;
        s_ww.afe_iface = &api;
        s_ww.feed_buffer = accumulator;
        s_ww.feed_chunk_size = chunks[c] * 3;
        consumed = 0;
        size_t submitted = 0;
        while (submitted < 80000) {
            size_t frames = batches[b];
            if (frames > 80000 - submitted) frames = 80000 - submitted;
            for (size_t i = 0; i < frames * 3; ++i) input[i] = sample(submitted * 3 + i);
            wake_word_feed(input, frames * 3);
            submitted += frames;
            assert(s_ww.feed_samples + s_ww.accumulator_remaining_samples == submitted);
            for (size_t i = 0; i < s_ww.feed_buffer_count; ++i)
                assert(accumulator[i] == sample(consumed + i));
        }
        assert(s_ww.feed_count == 80000 / chunks[c]);
        assert(s_ww.feed_fail == 0);
        printf("PASS chunk=%d batch=%d calls=%u remainder=%u\n", chunks[c], batches[b],
               s_ww.feed_count, s_ww.accumulator_remaining_samples);
    }
    return 0;
}
'''
gcc = shutil.which("gcc")
if not gcc:
    raise SystemExit("需要主机 gcc")
with tempfile.TemporaryDirectory(prefix="lummiss-afe-test-") as folder:
    cfile = Path(folder) / "afe.c"
    exe = Path(folder) / "afe.exe"
    cfile.write_text(test.replace("FUNCTION", function), encoding="utf-8")
    subprocess.run([gcc, "-std=c11", "-Wall", "-Werror", str(cfile), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
