"""编译并测试实际 C 重采样函数：双麦/参考槽位、同步相位和缓冲边界。"""
from pathlib import Path
import shutil
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] /
          "components/xiaozhi_audio/xiaozhi_audio.c").read_text(encoding="utf-8")
start = source.index("static void resample_mic_24k_to_16k(")
end = source.index("\n}\n", start) + 2
function = source[start:end]
gcc = shutil.which("gcc")
if not gcc:
    raise SystemExit("需要主机 gcc 来执行 C 函数测试")

test = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
FUNCTION
int main(void) {
    int16_t input[1440 * XIAOZHI_CAPTURE_CHANNELS];
    int16_t output[960 * XIAOZHI_AFE_CHANNELS + 2];
    for (int i = 0; i < 1440; ++i)
        for (int ch = 0; ch < XIAOZHI_CAPTURE_CHANNELS; ++ch)
            input[i * XIAOZHI_CAPTURE_CHANNELS + ch] = -12000 + ch * 6000 + i * 2;
    output[0] = 12345;
    output[960 * XIAOZHI_AFE_CHANNELS + 1] = 23456;
    resample_mic_24k_to_16k(input, output + 1, 960);
    for (int i = 0; i < 960; ++i) {
        for (int ch = 0; ch < XIAOZHI_AFE_CHANNELS; ++ch) {
            const int slots[] = {0, 2, 1};
            int expected = -12000 + slots[ch] * 6000 + (i / 2) * 6 + (i % 2) * 3;
            assert(output[1 + i * XIAOZHI_AFE_CHANNELS + ch] == expected);
        }
    }
    assert(output[0] == 12345);
    assert(output[960 * XIAOZHI_AFE_CHANNELS + 1] == 23456);
    /* 将同一段音频拆成六个 10ms 块，结果必须与原 60ms 块完全一致。 */
    int16_t split[960 * XIAOZHI_AFE_CHANNELS];
    for (int block = 0; block < 6; ++block)
        resample_mic_24k_to_16k(input + block * 240 * XIAOZHI_CAPTURE_CHANNELS,
                               split + block * 160 * XIAOZHI_AFE_CHANNELS, 160);
    for (int i = 0; i < 960 * XIAOZHI_AFE_CHANNELS; ++i)
        assert(split[i] == output[1 + i]);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="lummiss-mic-test-") as folder:
    folder = Path(folder)
    cfile = folder / "mic.c"
    cfile.write_text(test.replace("FUNCTION", function), encoding="utf-8")
    for raw_channels, afe_channels in ((4, 3), (4, 1), (1, 1)):
        exe = folder / f"mic_{raw_channels}.exe"
        subprocess.run([gcc, "-std=c11", "-Wall", "-Werror",
                        f"-DXIAOZHI_CAPTURE_CHANNELS={raw_channels}",
                        f"-DXIAOZHI_AFE_CHANNELS={afe_channels}",
                        str(cfile), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
        print(f"PASS: raw={raw_channels} -> AFE={afe_channels}, 960 samples/channel")

# AFE 每通道 1024 点：验证短采集块不再造成周期性的 120ms 供数间隙。
def max_feed_gap(chunk_samples):
    pending = 0
    feed_times = []
    for samples in range(chunk_samples, 16000 * 5 + 1, chunk_samples):
        pending += chunk_samples
        if pending >= 1024:
            pending -= 1024
            feed_times.append(samples / 16)
    return max(b - a for a, b in zip(feed_times, feed_times[1:]))

assert max_feed_gap(960) == 120
assert max_feed_gap(160) == 70
print("PASS: AFE 最大供数间隔 120ms -> 70ms（不含任务调度延迟）")
