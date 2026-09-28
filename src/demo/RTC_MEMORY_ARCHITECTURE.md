# 720p RTC 内存架构复核与修改（2026-09-24）

本报告以本轮开始时的事件刷新固件为“修改前”，不把历史已完成的 PSRAM 迁移重复计为本轮收益。保持 1280×720、20fps 上限、固定 QP36、不关闭 WDT、不降低 2048B 停止阈值。当前没有 H264 Guard。

**结论边界：已完成源码核对、定点迁移和固件构建；尚无本轮固件的板端运行数据，因此不能声称达到10分钟稳定或给出实测修改后 heap 数值。** 预编译 Peer 内部及驱动运行时并发峰值尚未全部量化，表中的“待测”不是零。

## 检索范围和分类

检索主工程 main/components/managed_components 及 build/compile_commands.json 中的 ESP-IDF 实际编译源文件，共3237个文件、1365个分配调用/声明候选。完整位置与表达式见 [allocation_sites.md](audit/allocation_sites.md)、机器可读结果见 [allocation_sites.json](audit/allocation_sites.json)。词法检索包含条件编译未启用的分支，不能将每行加总成当前占用；预编译 .a 的内部调用不能通过源码检索穷尽。以下是已人工核验的主路径分配表；未核验候选保留原状，不能伪称已完成全部动态归属证明。

- A / MUST_DMA：硬件直接访问。**不代表必须 INTERNAL**，P4 USB/JPEG/H264 的部分大数据允许 PSRAM，但必须保留各驱动的对齐及 cache 同步。
- B / INTERNAL_ONLY：cache关闭/ISR/内核或组件约束要求内部，但没有 DMA 直接访问依据。
- C / CPU_ONLY：可用 SPIRAM|8BIT。任务栈迁移须配套 WithCaps 创建/删除接口；不可直接改 ISR 使用的控制块。
- U / 未决：不凭模块名称猜，需继续由调用点/trace证明。

## 修改前后分配表

路径相对 src/demo；I=INTERNAL，D=DMA，P=SPIRAM，8=8BIT，CA=CACHE_ALIGNED。大小为请求或对齐后数据量，不含堆头。标准对齐表示普通分配器保证，并非指定16/64。表内“运行期”包括 RTC 会话。

|模块、文件/函数|大小|修改前 → 修改后 caps/落点|alignment|生命周期/RTC存在|类别、硬件直接DMA|必须I / 可P|提前固定及处置|
|---|---:|---|---|---|---|---|---|
|home_info/home_info.c:home_info_start，任务栈|8192|默认内部栈 → P\|8|栈要求|启动至关机/是|C/否|否/是|启动固定，已迁|
|main/camera_driver.c:camera_driver_run，handoff栈|4096|默认内部栈 → P\|8|栈要求|摄像头期/是|C/否|否/是|固定；失败路径改vTaskDeleteWithCaps|
|ambient_led/ambient_led.c:ambient_led_init，任务栈|4096|默认内部栈 → P\|8|栈要求|灯效启用期/条件|C/否|否/是|固定；RMT真实像素/驱动不改|
|mech_button/mech_button.c:button_init，任务栈|3072|默认内部栈 → P\|8|栈要求|按键启用期/条件|C/否|否/是|固定；GPIO轮询/当前回调只记录状态|
|touch_key/touch_key.c:touch_key_init，任务栈|3072|默认内部栈 → P\|8|栈要求|触摸键启用期/条件|C/否|否/是|固定；无ISR读取用户栈|
|webrtc_whip/rtc_heap_diag.c:s_caps，hook对照表|512×8=4096|静态L2 bss → RTCRAM\|8|标准|诊断启用期/是|B/否|是/否（hook可在cache关闭路径调用）|init固定；失败不回退DMA堆|
|managed esp_peer/src/esp_peer.c:esp_peer_open，wrapper|60（ELF sizeof）|calloc默认 → P\|8|标准|open至close/是|C/否|否/是|按会话分配；无需常驻|
|managed esp_h264/hw/src/esp_h264_enc_hw_param.c:new_param，SPS/PPS NAL|160请求，原I对齐192|I优先 → P，无I回退|cache对齐|编码器期/是|C/否|否/是|CPU生成后memcpy，已迁|
|managed esp_h264/hw/src/h264_rc.c:rc_new，RC统计|88 sizeof|I优先 → P，无I回退|cache对齐|动态RC时/当前固定QP不创建|C/否|否/是|迁移不计当前收益|
|H264:new_param，reference|92167请求，92224实际|I\|CA → 不变|16请求、内部cache对齐|编码器期/是|A/**是**|组件要求I/不擅改|实际编码器提前申请，无Guard|
|H264:single_hw_new，db_tmp|10319请求，约10368对齐|I\|CA → 不变|16/cache|编码器期/是|A/是|是/不改|随编码器建立|
|H264:new_param，deblocking db|1382416请求|I优先、失败P → 不变，历史实际P|8有效地址/cache|编码器期/是|A/是|组件允许P回退/是|未改硬件分配策略|
|H264:ref/db/mvm/dbtmp/yuv/bs描述符|10×16 sizeof，分别cache对齐|I\|CA → 不变|16/cache|编码器期/是|A/是|是/不改|随编码器建立|
|H264:hw handle/param handle|64/152 sizeof，外部对齐|P优先 → 不变|cache|编码器期/是|C/否|否/是|已在P|
|Hosted:mempool_set_fixed_limit，STA TX|2×1664=3328|按需最多2 → 网络通道创建时固定2|64|通道期/是|A/是|是/否|本轮提前分配；耗尽20ms等待，运行期不扩池|
|Hosted:sdio_mempool_create，本地空闲缓存|至多1×1664|不变|64|联网期/是|A/是|是/否|与STA合计空闲<=3，非固定池超额真实free|
|Hosted:sdio_rx_get_buffer，RX双缓冲|len随接收聚合，2份|I\|D\|8 → 不变|64|联网期，按最大长度增长/是|A/是|是/否|**不计入mempool计数**；新增独立容量/扩容日志|
|Hosted:SDIO寄存器缓冲|REG_BUF_LEN|I\|D\|8 → 不变|64|SDIO任务期/是|A/是|是/否|底层保留|
|Hosted:sdio_push_data_to_queue、RX协议副本|逐包长度|P\|8 → 不变|标准|逐包/是|C/否，DMA接收已完成|否/是|已有迁移，不重复计收益|
|Hosted:线程、锁、队列控制|配置栈/sizeof及队列深度|默认内部 → 不变|栈/标准|联网期/是|B/U，不能一概认为DMA|调用链未全证明/待核|保留并用trace补齐|
|USB:IDF hcd_dwc.c frame_list|FRAME_LIST_LEN×4|I\|D\|CA → 不变|USB要求|Host期/是|A/是|是/否|Host初始化固定|
|USB:传输descriptor list|desc_list_len×sizeof(qtd)|当前CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM=1|512/cache|URB期/是|A/是|否/驱动支持P|保持官方分配|
|UVC:uvc_host_stream_open，URB/帧|8×18KiB；3×512KiB（配置）|P/驱动caps → 不变|USB/cache|流期/是|A/驱动直接访问部分|按驱动/是|已有PSRAM路径|
|main/camera_driver.c:camera_frame_copy_pool_init|3×512KiB|P\|8 → 不变|标准|摄像头期/是|C/否|否/是|固定副本池|
|JPEG:IDF jpeg_decode.c rxlink/txlink|2×align(sizeof(desc)=24,cache)|I\|D → 不变|cache|decoder期/是|A/是|是/不改|创建decoder时固定|
|JPEG:header_info/decoder/trans_desc/ISR控制|header_info sizeof3196，其余sizeof|JPEG_MEM_ALLOC_CAPS → 不变|标准|decoder/事务期/是|B/U，ISR/cache约束需保留|驱动约束/未迁|不能仅因header名字认定CPU-only|
|video_streamer:JPEG输出|1280×720×2=1843200|驱动P分配 → 不变|cache|codec期/是|A/JPEG写|否/是|固定|
|video_streamer:YUV420输入|1280×720×1.5=1382400|P → 不变|cache|codec期/是|A/H264读，CPU转换也写|否/是|固定|
|video_streamer:H264输出槽|4×约131200|P → 不变|128/组件cache|codec期/是|A/H264写|否/是|固定，不扩大队列|
|video_streamer:输入/输出索引、控制队列|3/4/控制深度×4及控制块|FreeRTOS默认 → 不变|标准|模块期/是|B控制块/C载荷|控制块保留/载荷可P|仅几十字节，先保留|
|LCD:lvgl_port_add_disp，draw双缓冲|2×1920=3840|I\|D → 不变|驱动要求|UI期/是|A/是|是/不改|固定|
|LCD:IDF SPI setup_dma_priv_buffer|单次最大1920+对齐、并发由队列决定|I\|D → 不变|DMA/cache|flush事务/事件时|A/是|是/否|RTC<=30s事件刷新；不足余量延后|
|LVGL:控件/动画管理队列|控件动态；表情载荷192|控件由LVGL分配；载荷P → 不变|标准|UI期/是|C/否|否/是|前轮队列已迁，控制块留I|
|animation:SD fread/索引/三帧|4096/最多12288/3×153600|P\|8 → 不变|标准|仅动画期/RTC释放|C/否|否/是|RTC停止且释放，无常驻大帧|
|I2S:ring buffers TX/RX|6×240×2×2=5760；四槽RX约11520|驱动I\|D → 不变|DMA|音频期/是|A/是|是/否|驱动分配固定，另计descriptor/控制块|
|xiaozhi_audio:采集用户PCM/Opus/播放PCM|按encoder_input_size、codec比率、池深度|P\|8 → 不变|标准|音频期/是|C/否，I2S内部memcpy|否/是|前轮已迁|
|xiaozhi_audio:wake/feed/preroll/AFE|feed/preroll显式caps；AFE预编译内部分配|按现有配置 → 不变|组件要求|音频期/是|C/U|AFE约束需保留|不得假定所有AFE buffer可P|
|MCP:消息队列/单消息/凭据/拍照上下文|消息3072×深度；其余sizeof|P\|8 → 不变|标准|任务或请求期/是|C/否|否/是|已有PSRAM路径|
|MCP:电机任务参数|12|P\|8 → 不变|标准|动作创建短暂/条件|C/否|否/是|前轮迁移，不计本轮|
|WHIP:offer/answer、credential URL/body|SDP各16384，其他见常量|P\|8 → 不变|标准|握手期/瞬态|C/否|否/是|HTTP结束释放offer/answer/客户端，不常驻|
|Peer:media_lib malloc/calloc/realloc|依协议对象|P\|8 → 不变|标准|会话期/是|C/否|否/是|前轮已迁|
|Peer:预编译库其余存活块|旧trace17184请求，不全归属库|未知/默认/内核混合 → 不变|各调用|open窗口其他任务也可能申请|U|需caller核实|本轮>=1KiB存活块输出caller/caps，不盲迁|
|MQTT:IDF客户端input/output/控制|4096/1024 + sizeof|默认malloc → 不变|标准|MQTT长期/是|C为主/否|一般可P，但SDK全局未改|待独立组件allocator修改；不改全局malloc阈值掩盖|
|JSON:cJSON对象/打印文本|变长|默认hooks → 不变|标准|业务消息短暂/是|C/否|可P|全局hook会涉及多模块，未在本轮盲换|
|cloud_udp:GCM/消息环、控制|sizeof/静态容量|P或现有分配 → 不变|加密接口要求|音频UDP期/是|C/U加密底层另计|按调用链/已P部分|保留加密硬件约束|
|sd_card:sdmmc_card_t/slot/block_buf|200/sizeof/512等驱动常量|默认控制对象/I\|D block_buf → 不变|标准/DMA|SD挂载期/条件|C控制/A bounce|分别处理|不改正在验证过的SDSPI驱动|
|诊断：heap trace记录/任务快照|512×sizeof(trace)；64×sizeof(TaskStatus)|P\|8 → 不变|标准|trace短期；任务快照长期/是|C/否|否/是|hook表单独归B|

### 本轮收益（申请需求，不等同于heap实测增量）

|项目|减少内部需求|条件|
|---|---:|---|
|5个普通任务栈|22528B|所有上述任务创建时；核和优先级不变|
|hook对照表|从L2移走4096B，新增4B指针及少量诊断状态|仍占RTCRAM，不是减少全部INTERNAL|
|Peer wrapper|60B请求|每会话；实际heap节省含对齐待测|
|H264 NAL|192B原内部对齐数据量|编码器存在时|
|H264 RC|本配置0B|固定QP36不创建RC对象，不能虚计88/128B收益|

合计普通PSRAM迁移为22528B任务栈+60B wrapper请求+192B NAL对齐数据；另将4096B诊断表移出DMA-capable L2。**不能直接将这些数字加到DMA largest上**，是否能合并连续块取决于heap布局和分配顺序。

## H264 reference与heap区域

本地IDF证据：`D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/port/esp32p4/memory_layout.c`。

|区域|能力|容量/限制|结论|
|---|---|---|---|
|L2 RETENT_RAM/RAM，0x4ff…|INTERNAL、DMA、SIMD等|按cache和静态段扣减|92KB reference正常来自这里|
|RTCRAM，0x50108000附近|INTERNAL、RTCRAM，无DMA|物理约32KB，当前free更小|无法容纳92KB；用于4KB cache-safe诊断表|
|SPM，0x30100000|INTERNAL、SPM，无DMA|物理8KB，还需扣启动段|无法容纳92KB；不支持128bit SIMD访问|
|PSRAM，0x48…|SPIRAM、8BIT等；部分专用DMA可访问|32MB|不能据此推断reference可用|

旧日志的实际reference地址 **0x4ff99040**、实际92224B，明确位于L2。新固件新增 `H264_REF_REGION` 输出实际地址，不能在未运行时宣称新地址。

`esp_h264_enc_hw_cfg_dma_db_ref()` 把 `param->ref` 写入 `cfg_dsc(...H264_DMA_OWNER_H264...)` 并调用 `h264_dma_hal_cfg_ref_dsc`，因此reference归A，不因没有MALLOC_CAP_DMA标志就归B。分配的caps是正向能力集合，没有“INTERNAL且禁止DMA区域”的通用减法caps。虽可用RTCRAM/SPM指定小区域，但两者均不足，且不是可替换的H264硬件目标。

当前esp_h264 1.4.1的`esp_h264_enc_cfg_hw_t`没有allocator callback；分配封装在port/src/esp_h264_alloc.c，本地改实现属于组件修改，不是用户级allocator hook。改变heap priority不会增加一个92KB非DMA区，也不能把不支持硬件访问的区域变为可用。本轮不改IDF全局heap priority、不将reference迁PSRAM。

## 200000B内部保留量与预算

`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=200000`，即195.3125KiB；`ALWAYSINTERNAL=16384`。IDF `esp_psram_extram_reserve_dma_pool()` 从DMA|INTERNAL取块后，用DMA|INTERNAL优先caps重新注册。它不是DMA专用隔离区，普通明确申请INTERNAL的对象仍可进入，且本身可以由多个不连续块组成。

已知720p必要量：reference92224、db_tmp约10368、10个H264 descriptor的对齐量约640、I2S纯数据约17280、LCD draw3840、STA固定3328、SDIO本地空闲至多1664、JPEG RX/TX descriptor约2个cacheline。仅这些已超过129KB，**尚未包含**Hosted RX双缓冲、Hosted/USB/系统任务栈、USB控制结构、JPEG header/control、Peer其余内部分配、内核对象、并发SPI bounce、对齐与heap开销。不能把这个小计当完整峰值。

隐藏预算：Hosted RX streaming双缓冲不计入原POOL current_bytes，且随接收长度增长并保留。本轮补 `SDIO_RX_DMA` 和 `SDIO_RX_GROW`，防止以“pool仅4992B”误判Hosted总体用量。

**建议本轮保留200000B，不改128KB/256KB。** 最终值需用本固件10分钟与多次START/STOP的真实峰值确定：同时存活的A+B+不能迁移的默认内部量+最大并发运行期DMA请求+对齐/碎片余量；已有分配不可与“阶段free下降”重复加总。增加reserve不会增加L2物理容量，也不保证92KB连续块。

8192B warning和2048B stop仍保持原值；目标largest>8KB、最好16KB是验收目标，不是已经计算完成的严格下界。LCD事件准入 free>=8192且largest>=4096仅减少风险，不保证并发malloc绝不失败。

## Hosted固定/缓存池

旧日志（a3d99a35附件）pool_peak_blocks=2、cached=3、current_bytes=4992、wait_timeout=0。当前代码已对STA设置两个lease，故这是**受限并发下的实测**，不是无限队列下的天然峰值。本轮保持N=2，不虚称最优值。

- STA通道创建时申请2×1664，全部成功才返回可用通道；部分失败由destroy回收并清空全局通道入口。统计计数包含这次预热，不能把预热峰值当作运行期自然峰值。
- 修复通道重注册时仅free通道而丢失旧mempool的路径：改为复用同一池、更新回调，避免固定块泄漏及借出块归还到新池。
- 运行期lease最多2，耗尽最多等待20ms，已有固定池不再向heap扩容。借出块只能由原发送完成路径归还，未改变SDIO DMA所有权。
- 本地SDIO保留至多1个空闲块，其余通道cache=0；全局CAS空闲上限3继续有效；降低上限时立即移除多余空闲节点，释放在spinlock之外。
- **cached<=3并不要求每次运行real_free_count必然>0。** 若始终只使用这三块、没有多余块，0是正常的固定池复用结果；不能人为malloc/free刷计数。超额时才应见real_free增长。
- 本轮没有把RX双缓冲或所有控制通道改为固定池：聚合接收最大长度尚缺数据，盲目固定N或缩小会引入丢包。它们单列预算，不受STA两块限制。

## RTC生命周期、LCD与CPU

保持已验证的“实际编码器早于Peer”顺序，原因是Peer之后再申请92KB可能因碎片失败；本轮没有证据证明后移可以保证成功，因此不机械采用建议顺序。真实顺序：

1. STARTING关闭LCD周期提交；UI转HOME，animation停止并释放帧/索引/read buffer；两动画定时器暂停。
2. 网络通道已经建立STA真实固定池；按需创建视频pipeline和**实际**H264编码器。
3. 首次预览启动UVC，等有效帧CAMERA_READY；后续会话复用已运行UVC。
4. credential HTTP → Peer open → ICE → WHIP HTTP → DTLS；阶段heap分别采样。
5. credential客户端/body/url及时释放；WHIP answer/offer/client在HTTP流程结束释放；DTLS/Peer连接上下文需要保持直到STOP，不应误当泄漏。Peer残余具体归属用新增caller日志确认。
6. STREAMING只开启视频门控；LVGL有内容变化才刷新，RTC最多每30秒一次且DMA不足保留事件；animation任务永久等通知，不接新请求。
7. STOP关闭视频门控、DELETE WHIP、close Peer、清会话对象；恢复UI。codec空闲轮询随后实际释放H264，单独打印AFTER_H264_FREE，不能用STOPPED替代此事实。

Guard最终设计：**0B，不存在**。不是晚建/缩小；没有Guard释放给reference的瞬间窗口，不恢复旧96KB方案。固定STA池由实际SDIO发送使用，与Guard不同。

|任务|core/priority|栈及位置（本轮后）|等待/调度|
|---|---|---|---|
|video_codec|CPU1/8|8192 P|输入队列最长50ms；每帧显式vTaskDelay(1)，防止满队列永远READY|
|taskLVGL|CPU1/4|7168 P|LVGL任务保留；RTC只内容事件刷新，最多30s一次|
|anim_player|CPU1/7|8192 P|RTC资源释放后通知阻塞；5ms和50ms相关timer均暂停|
|xiaozhi_dec / wake_process|CPU1/9、8|20480 P / 40960 P（既有配置）|音频队列/AFE等待，音频不改|
|video_report|CPU1/4|4096 P|每5s统计|
|video_upload|CPU0/9|8192 P|输出队列阻塞、发包有界|
|USB/UVC / usb_events|CPU0/20、19|驱动栈 / 4096内部|驱动等待；未迁USB栈|
|camera_handoff|CPU0/18|4096 P，本轮迁|队列portMAX_DELAY|
|camera_task|CPU0/7|8192 P|安装/重连/等待|
|webrtc_whip|CPU0/4|12288 P|控制队列/peer循环|
|xiaozhi_mic/spk|CPU0/6、10|40960/6144 P|I2S/队列等待|
|home_info|CPU0/0|8192 P，本轮迁|本地时间/后台HTTP退避，HTTPS当前暂停|
|ambient_led/button/touch|原未绑核/3、4、4|4096/3072/3072 P，本轮迁|周期延时；不迁核|
|ESP-Hosted、lwIP、系统任务|维持驱动配置|内部保留|具体任务运行量未全部测出，见检索清单|

没有降低分辨率、没有扩大队列、没有关WDT、没有把CPU1任务挪到CPU0。编码逐帧耗时已由JPEG_DEC/YUV_CONV/H264_ENC/SEND统计；每帧1tick阻塞不是20fps保证。若总处理>50ms，吞吐自然低于20fps。

RC复核：h264_rc.c已存在有符号int64计算 `(bits_per_frame*10 - average*4)/6` 并限幅[1,INT_MAX]，原无符号下溢已修；当前qp_min=qp_max=36根本不创建RC对象，所以旧unsigned_sub_underflow日志不能作为本固件当前RC状态。固定QP并不承诺目标码率，未改变画质/bitrate策略。PLI/IDR和SEND统计保留。

## 已知修改前数据与新固件验证

旧a3d99a35日志：

|阶段|DMA free/largest B|
|---|---:|
|BEFORE_H264_OPEN|144579/110592|
|H264_REF_ALLOC之后|52155/20480|
|完整H264打开后|40871/19456|
|UVC_READY后的RTC_START|23871/19456|
|稳定推流部分时段|2099/2048|

另一次ee62d06b日志在1675/1632触发STOP。以上**不是紧邻本轮修改前同负载A/B**，只能作历史参照。新固件free/largest、持续时长、稳定FPS、各错误计数全部待板端日志；没有填写猜测值。

新日志：H264_REF_REGION；FIXED_DMA_POOL ready；SDIO_RX_DMA/GROW；PEER_LIVE（>=1KB内部存活块，size/caps/ptr/caller）；STREAMING_MIN、RTC_MEMORY_MIN（1s采样最低值）。阶段日志保留LVGL_READY、CAMERA_READY、RTC_START、BEFORE/AFTER_PEER、ICE、DTLS、H264前后、RTC_STOP、AFTER_H264_FREE。

minimum是控制循环约1秒和阶段采样的最低值，阻塞HTTP期间不是严格1Hz；不能声称覆盖所有微秒级峰值。首次DMA申请失败另外即时捕获现场，并修复“先发布ready后填快照”的跨核竞争。

验证步骤：用IDF扩展烧录src/demo/build；冷启动保存全日志，在浏览器持续预览至少600秒，再STOP/START两次。确认画面持续变化；核对fixed pool ready=2/3328、cached<=3、pool alloc_fail/tx_wait_timeout=0、无DMA_BUDGET_CRITICAL/各驱动alloc fail/WDT/reboot；记录minimum并检查largest持续>8192（目标最好>16384）。如果Peer存活大块仍在I，使用**同一ELF**解析PEER_LIVE caller后再迁移，不能根据大小猜来源。

**10分钟测试结果：待用户烧录回传，未执行，未通过声明。** CPU峰值、全部第三方动态分配归属、RX双缓冲上界和最终reserve预算仍需这个测量闭环；本报告不把未决项写成已解决。

managed_components中的改动会被组件更新覆盖，应在升级时保留并复核mempool、sdio_drv、esp_peer、h264 NAL/RC/诊断补丁；本轮未修改SDK全局heap实现。

## 本轮构建

Ninja构建及分区尺寸检查通过。ELF SHA256：`c0060cf856dc9c19f2643731e6acc11126dd0aa4eadeef0f258fcbae0d21c8a8`。输出`build/lummiss_main.bin`，未烧录。改动源码指纹见`audit/firmware_sources.json`；构建记录见`build_rtc_memory_arch.log`。
