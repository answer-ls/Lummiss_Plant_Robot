# ESP-IDF v5.5.5 DMA / INTERNAL reserve 源码审计

审计日期：2026-09-28。范围：只读源码、官方版本比较及现有运行证据；不修改 IDF、业务代码或 Kconfig，不编译、不烧录。

## 结论

**本地 v5.5.5 确实包含 commit 378ecdb 引入的 reserve DEFAULT 可见性变化。当前 `ALWAYSINTERNAL=16384` 会让小块普通 malloc 在尝试 PSRAM 之前申请 INTERNAL，其中包含 reserve pool。** 这不是根据 issue 猜测，而是由注册 caps、malloc 分支和 allocator 匹配条件共同证明。

但是，BACKWARD_CHAIN 中 newlib mutex、FreeRTOS queue/TCB 等使用显式 `INTERNAL|8BIT`，即使 v5.5.3 没有 DEFAULT，它们仍可进入 reserve。不能把所有非 DMA 对象归咎于新增 DEFAULT，也不能承诺撤回 commit 或改变 malloc 偏好就能使 H264 成功。

还有 P4 特有的优先级细节：普通 RETENT_RAM 和 reserve 的 INTERNAL 匹配都在 priority 1；reserve 动态注册时插到 heap 链表头。`DEFAULT|INTERNAL` 请求可因 INTERNAL 位在 priority 1 就选中 reserve，而不是必须等到 DEFAULT 所在 priority 2。因此“只有所有其他 INTERNAL 耗尽才回落 reserve”并不是该 P4 实现的严格保证。

当前官方配置候选是只把 `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` 从 16384 改为 0，继续保持 reserve=200000。**本轮没有实施。** 它只改变普通 malloc/calloc/realloc 的选择策略，不搬迁已分配对象，也不改变显式 INTERNAL/DMA 或直接 DEFAULT caps 申请。

## 1. 来源、版本及可复核性

- 本地 IDF：`D:/espidf5.5.5/.espressif/v5.5.5/esp-idf`。
- 本地 HEAD：`b774170ff46c393eeb5e495ea37936038d3f4f4f`，标签 v5.5.5；本轮检查 `git status --short` 无输出。
- issue：[18938 / IDFGH-18095](https://github.com/espressif/esp-idf/issues/18938)，当前 open。报告者实际使用 ESP32-S3-WROOM-1-N8R2，不是 P4；本项目的 P4 结论单独从源码推导。
- commit：[378ecdbeb4941ea9b94423f7c8c3625b40d4607c](https://github.com/espressif/esp-idf/commit/378ecdbeb4941ea9b94423f7c8c3625b40d4607c)，是合并提交，父提交为 `10ca0dff2f40aeb539f22d1ef88d031cf7421743`、`31ce82f7d626671ec9d955b7cad62f9e2e2b83c8`。
- 本次 `git ls-remote` 查询到的 release/v5.5 HEAD：`2553c5ad432927e09ccbbad3b954e1cbc4bd7655`，提交时间 2026-09-21。以下“最新分支”均固定指这个 SHA，不推断未来分支状态。
- 官方原始文件、API issue/comments/commit metadata、本地源码副本、SHA256 和版本 diff 均保存于 [logs/idf_555_dma_reserve](./logs/idf_555_dma_reserve/)。其中 `sources.json` 记录 28 个远端源文件的 URL/hash，`version_diff.txt` 保存完整文件差异。
- 对比的 7 个关键文件，本地与官方 v5.5.5 文本完全一致：esp_psram.c、Kconfig.spiram.common、heap_caps.c、heap_caps_base.c、heap_caps_init.c、P4 memory_layout.c、freertos/heap_idf.c。

官方维护者确认小 malloc 的 reserve 消耗属于回归，并建议把 ALWAYSINTERNAL 调低到 0；后续讨论偏向改变分配优先策略，而非直接撤回 DEFAULT。该讨论是策略参考，不替代源码验证。[维护者说明](https://github.com/espressif/esp-idf/issues/18938#issuecomment-5355365443)、[后续说明](https://github.com/espressif/esp-idf/issues/18938#issuecomment-5578317280)。

## 2. reserve 如何创建和注册

以下行号均针对本地 v5.5.5。

1. `components/freertos/app_startup.c:174-179`：main_task 回收启动栈后、调用 app_main 前，把 `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` 传给 reserve 函数；失败则 abort。
2. `components/esp_psram/system_layer/esp_psram.c:635-660`：`esp_psram_extram_reserve_dma_pool(size)` 取 DMA|INTERNAL 的 largest，以 `min(largest, remaining)` 从现有 heap 分配父块。
3. 分配调用为 `heap_caps_malloc(next_size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`。
4. 将父块以独立子 heap 注册，起点 `dma_heap`，终点参数 `dma_heap + next_size - 1`。若无法一次取足，会循环创建多个不连续 pool；reserve 配置值不保证单个连续块。
5. `components/heap/heap_caps_init.c:282-341`：创建 heap_t（INTERNAL|8BIT），拷贝 caps，执行 `multi_heap_register(start, end-start)`，设置锁，最后 `SLIST_INSERT_HEAD` 注册。

这不是新增物理 RAM，也不是给 H264 独占保留的 guard。父 heap 中的块被占用，其内部交由子 heap 管理。

注册的三级 caps：

| 优先级索引 | caps |
|---|---|
| 0，高 | 0 |
| 1，中 | DMA、INTERNAL |
| 2，低 | DEFAULT、8BIT、32BIT |

caps 是跨三级取 OR 后判断能否满足请求；不是必须在同一级找到请求的全部位。该 pool 不具备 SPIRAM、RETENTION、SIMD 等未登记能力。CACHE_ALIGNED 还会经过硬件对齐调整，不能只看这些原始位就保证特定分配成功。

源码：[reserve 注册](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_psram/system_layer/esp_psram.c#L635)、[heap 注册](https://github.com/espressif/esp-idf/blob/v5.5.5/components/heap/heap_caps_init.c#L282)。

## 3. 哪些 API 能使用 reserve

| 请求 | 能否进入当前 reserve | 原因 / 优先级 |
|---|---|---|
| 普通 malloc，0<size<=16384 | 能，而且可先于 PSRAM | 首次请求 DEFAULT|INTERNAL，reserve 满足；此尝试成功便不再尝试 PSRAM |
| 普通 malloc，size>16384 | 能，但通常是 PSRAM 失败后的 fallback | 首次 DEFAULT|SPIRAM；失败后 DEFAULT |
| heap_caps_malloc(size, DEFAULT) | 能 | 不经过 ALWAYSINTERNAL 分支；reserve 在 priority 2 匹配 |
| heap_caps_malloc(size, INTERNAL) | 能 | priority 1 匹配，DEFAULT 是否存在不影响此请求 |
| heap_caps_malloc(size, INTERNAL|8BIT) | 能 | priority 1 有 INTERNAL，跨级 OR 也有 8BIT |
| heap_caps_malloc(size, DMA) | 能 | priority 1 匹配 |
| heap_caps_malloc(size, DMA|INTERNAL) | 能 | priority 1 匹配 |
| heap_caps_malloc(size, SPIRAM|8BIT) | 不能 | reserve 没有 SPIRAM |

这里“能”表示 capability 匹配；实际还需足够的连续空间、对齐和成功的底层分配。

### malloc 的真实调用链

`components/newlib/src/heap.c:22-24, 36-46, 65-67` 中 malloc/calloc 的包装进入 `heap_caps_malloc_default()`；realloc 使用对应 default 实现。

`components/heap/heap_caps.c:95-132`：

- 外部默认分配尚未启用时，只请求 DEFAULT|INTERNAL。
- 启用后，size<=limit 请求 DEFAULT|INTERNAL，否则 DEFAULT|SPIRAM。
- 首次失败后，重新以 DEFAULT 请求，而不是无条件只指定另一种内存。
- `esp_psram.c:141` 用 Kconfig 的 ALWAYSINTERNAL 调用 `heap_caps_malloc_extmem_enable()`。

当前两个构建的 `config/sdkconfig.h` 均为：USE_MALLOC=y、ALWAYSINTERNAL=16384、RESERVE_INTERNAL=200000；项目已开启启动阶段 PSRAM 初始化。

### 匹配算法为何重要

`heap_caps_base.c:143` 起按 priority 从 0 到 2 遍历 registered_heaps。某一级只需与请求有任意共同位，再检查整个 heap 的总 caps 是否包含所有请求位。

因此 reserve 对 `DEFAULT|INTERNAL` 在 priority 1 已可尝试；不能说 DEFAULT 在低优先级，就保证复合请求最后才使用它。

P4 `memory_layout.c:64-71` 将普通 RETENT_RAM 的 DEFAULT/INTERNAL/DMA 放 priority 1；RAM 放 priority 0；SPIRAM 的 SPIRAM 位放 priority 0、DEFAULT 放 priority 2；SPM/RTCRAM 的通用 INTERNAL 能力在 priority 2。

reserve 注册到链表头后，在相同 priority 上会先于原有 RETENT_RAM 尝试。因此：

- 小 malloc 的 INTERNAL 首次尝试根本不会选 PSRAM；reserve 可在普通 RETENT_RAM 尚有可用空间时被尝试。
- 单独 DEFAULT 请求可先选普通内部 heap，再在 priority 2 比较 reserve/PSRAM；同级依注册链表次序，不存在“DEFAULT 天然优先 PSRAM”的规则。
- 不能把文档中其他 INTERNAL exhausted 理解为每一块内部 heap 都已零空闲；分配尺寸和连续性也影响是否失败。

源码：[default malloc](https://github.com/espressif/esp-idf/blob/v5.5.5/components/heap/heap_caps.c#L107)、[caps 匹配](https://github.com/espressif/esp-idf/blob/v5.5.5/components/heap/heap_caps_base.c#L143)、[P4 优先级](https://github.com/espressif/esp-idf/blob/v5.5.5/components/heap/port/esp32p4/memory_layout.c#L64)。

## 4. 0x4ff861c0 region 的身份

现有证据共同支持它是 A=200000 的 reserve 子堆：

- 配置和已保存 A ELF 的反汇编明确传入 0x30d40=200000，调用 reserve；见 `logs/reserve_internal/A/reserve_call.txt`，不是只看配置文件。
- 上述代码从父 INTERNAL/DMA heap 取块后注册嵌套 heap。现有布局的该地址处于父 heap 范围内，符合这一行为。
- 注册起点 0x4ff861c0，终点参数 0x4ffb6eff；`end-start=199999`，与 summary 完全符合。父申请长度 200000，与注册 length 的差 1 来自上述传参/注册算式。
- 项目 main/components 没有其他 heap_caps_add_region 调用；已保存 map 把 reserve 实现关联到 esp_psram.c.obj。

证据边界：map 不能单独证明动态地址；没有新增断点直接读取 dma_heap 返回值。因此这是启动实现、ELF参数及运行布局的交叉确认，不伪称直接观测到了函数返回指针。地址是该基线布局的地址，不是 IDF 固定地址。

## 5. BACKWARD_CHAIN 的非 DMA 对象为何进入

| 对象 | 已确认分配路径 | 与新增 DEFAULT 的关系 | ALWAYSINTERNAL=0 是否直接改变 |
|---|---|---|---|
| newlib lock | locks.c:76 → xQueueCreateMutex → pvPortMalloc → INTERNAL|8BIT | 无需 DEFAULT；旧版也能使用 reserve | 否 |
| FreeRTOS TCB、默认内部任务栈 | pvPortMalloc / 内部栈创建路径 | 无需 DEFAULT | 否；MQTT 6KiB默认任务栈也不自动迁移 |
| 动态 queue / mutex / semaphore | FreeRTOS 动态分配 → INTERNAL|8BIT | 无需 DEFAULT，不是 libc malloc 偏好 | 否 |
| LVGL 主题/样式等核心软件对象 | 当前 lv_mem.c:27-29 已显式 SPIRAM|8BIT | 当前已绕过普通 malloc，不应再次列为本轮新增收益 | 否，已在 PSRAM |
| LVGL port 的锁、TCB、LCD 控制对象 | RTOS INTERNAL 或驱动显式 caps，须分别看 owner | 不等于 LVGL 核心主题对象 | 对显式 caps 无效 |
| 普通业务 / 面板 / 文件系统等 libc malloc/calloc 对象 | 只有解析到普通 allocator 的对象才属于此组 | 新 DEFAULT 允许小对象首次 INTERNAL 分配进入 reserve | 是，后续普通分配先尝试 PSRAM |
| I2S/LCD/SDSPI DMA、内部中断对象 | 显式 DMA/INTERNAL | pool 的预期用户，不是 malloc 回归证据 | 否，应保留 |
| 未知 owner | 当前 trace 未解析完整 | 不下结论 | 未知 |

`freertos/heap_idf.c:43-57` 明确把 pvPortMalloc 映射为 INTERNAL|8BIT。newlib 的“普通锁”在语义上是软件对象，但分配策略上属于 RTOS 内部对象，不能只因不执行 DMA 就迁走。

新增 DEFAULT 扩大了竞争者范围，加上 P4 优先级调整，能解释碎片风险增加；但当前没有逐对象的旧版同负载对照，无法量化 issue 导致了链中多少字节。不能说 52 个对象都是此次回归产生。

## 6. v5.5.3、v5.5.5、最新 release/v5.5 对比

| 相关实现 | v5.5.3 | v5.5.5 / 本地 | release/v5.5 @2553c5ad |
|---|---|---|---|
| reserve 低优先级 caps | 8BIT、32BIT | 新增 DEFAULT | 与 5.5.5 相同 |
| reserve DEFAULT|INTERNAL 能否匹配 | 否 | 是 | 是 |
| reserve INTERNAL|8BIT / DMA 能否匹配 | 是 | 是 | 是 |
| ALWAYSINTERNAL 默认值 | 16384 | 16384 | 仍为 16384 |
| default malloc 的大小阈值/fallback策略 | 同当前 | 同旧版 | 未变 |
| heap_caps_base 匹配算法 | 同当前 | 同旧版 | 未变 |
| P4 RETENT_RAM 通用 caps | priority 0 | priority 1 | 同 5.5.5 |
| P4 TCM/SPM 通用 INTERNAL caps | priority 1 | SPM priority 2 | 同 5.5.5 |
| FreeRTOS pvPortMalloc caps | INTERNAL|8BIT | 不变 | 不变 |

378ecdb 的直接相关更改不仅是 reserve 添加 DEFAULT，还包括 P4 RETENT_RAM/SPM 的优先级变化，以及同步更新 Kconfig/API 文档；不能只撤一行就当成完整还原旧版本行为。

5.5.3→5.5.5 另有 PSRAM 加密豁免区等变动；heap_caps.c 的其他变化在最小空闲量监控，heap_caps_init.c 增加启动阶段/minimum free 处理。它们不改变本报告证明的默认 malloc 分支或 reserve caps。完整差异保存在 version_diff.txt。

5.5.5→本次分支 HEAD：这 7 个文件中只有 esp_psram.c 不同，差异是在 esp_psram_init 开头对重复初始化返回 INVALID_STATE；reserve 函数未变，其余 6 文件完全一致。**维护者讨论的默认值改为 0，并未体现在本次核对到的 release/v5.5 Kconfig。不能声称升级到该分支就已修复。**

版本源码：[v5.5.3](https://github.com/espressif/esp-idf/blob/v5.5.3/components/esp_psram/system_layer/esp_psram.c)、[v5.5.5](https://github.com/espressif/esp-idf/blob/v5.5.5/components/esp_psram/system_layer/esp_psram.c)、[固定的 release SHA](https://github.com/espressif/esp-idf/blob/2553c5ad432927e09ccbbad3b954e1cbc4bd7655/components/esp_psram/system_layer/esp_psram.c)、[该 SHA 的 Kconfig](https://github.com/espressif/esp-idf/blob/2553c5ad432927e09ccbbad3b954e1cbc4bd7655/components/esp_psram/Kconfig.spiram.common)。

## 7. 官方配置方式及明确边界

候选配置组合为 USE_MALLOC=y、ALWAYSINTERNAL=0、RESERVE_INTERNAL=200000。前提是 PSRAM 已完成初始化、纳入 heap，且 default allocator 已启用外部内存。

效果：所有非零大小的普通 malloc 首先请求 DEFAULT|SPIRAM。成功便不会竞争 reserve；失败仍会以 DEFAULT fallback。因此这是偏好策略，不是普通 malloc 永久禁止进入 reserve 的硬隔离。

不受影响：显式 INTERNAL/DMA 请求仍可使用 reserve，FreeRTOS 动态控制对象仍 INTERNAL，真正硬件 DMA buffer 不被强制迁走。直接 heap_caps_malloc(DEFAULT) 绕过大小阈值，仍按 heap priority 分配。此配置不会回迁/搬移现有对象。

如需某个 CPU-only 对象严格放 PSRAM，应在该对象的受控 allocator 使用 SPIRAM|8BIT；当前 LVGL 核心对象已经这样处理。不能把普通 malloc 改成优先 PSRAM 当作所有调用者都已通过 cache-disabled 访问审核，仍需实机验证。SPIRAM_USE_CAPS_ALLOC 使普通 malloc 默认留在内部，不是本目标的替代方案；RESERVE_INTERNAL=0 则取消 reserve，也不是保护 DMA 的方案。

## 8. 下一步最小实验建议（未执行）

相比继续扩大 reserve 或先迁 MQTT 栈，更直接的候选是固定 reserve=200000，只测试 ALWAYSINTERNAL：A=16384，B=0。无需 IDF patch。已有诊断足够，先不新增 heap 打印。

沿用现有冷启动、WAKE_IDLE、RTC_START、AFE释放后、H264_OPEN_BEFORE 的 free/largest 与 FRAG owner 证据，验证 REF 实际 92224B 分配成功、地址非空；不能仅凭 INTERNAL total 上升判定成功。

同时验证 Hosted、USB、I2S、LCD、SD、语音及 RTC 的正常运行。若能 STREAMING，再做 60 秒预览和 10 次 START/STOP。若 largest 仍不足，应查看还存活的显式 INTERNAL/DMA 链；不自动迁 TCB/mutex/DMA，也不自动增大 reserve。

当前报告结论是“源码确认存在相同分配机制，且有官方配置候选”，不是“H264 已修好”。本轮没有改配置、没有迁 MQTT 栈、没有改分辨率或音频链。
