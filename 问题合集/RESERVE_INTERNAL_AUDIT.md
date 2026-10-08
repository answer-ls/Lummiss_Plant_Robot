# INTERNAL reserve 单变量审计

## 当前配置恢复

按用户后续要求，已将src/demo/sdkconfig、build_rtc_mem_b/sdkconfig、sdkconfig.defaults恢复200000，并重新构建。MQTT只做审核，未迁栈；详见MQTT_STACK_PSRAM_AUDIT.md。下文“源码仍为229376”仅描述B日志分析当时状态，已被本节覆盖。

## B版冷启动实测：失败，不合格

最新日志保存为 `logs/reserve_internal/B_boot_failure.txt`。板端ELF摘要79407c370与B_manifest中RTC构建79407C370DED...匹配，确认运行的是本次224KiB实验固件。

时间线：HOSTED_INIT_DONE时INT277856/largest237568、DMA238756/largest237568；随后创建/运行main_task，1460ms请求224K reserve，1468ms返回0x101（ESP_ERR_NO_MEM），freertos/app_startup.c:179显式abort并重启。尚未调用app_main，也未初始化业务AFE/LCD/SD或进入RTC/H264申请。因此B在启动验收第一关已经失败，不能保留为正式配置。

此前237568是Hosted阶段采样，不是reserve内部实际分配瞬间；期间存在任务创建/调度。该日志不能区分reserve函数中的DMA块申请失败、next_size为0，或heap_caps_add_region_with_caps返回NO_MEM（包含注册控制对象申请失败）。也没有打印chunk数量、实际申请地址，不能给出B region长度/边界或largest增长的实测值。

更新结果：A=200000能启动但H264失败；B=229376启动reserve失败、H264未执行；USB/UVC、I2S、LCD、SD、AFE、60秒及十轮全部未测，Hosted任务启动不等于无线通信通过。明确不是本轮H264 runtime failure，也不是日志证明的WDT或掉电。

建议恢复200000作为可启动基线；当前不测试更大的232/240KiB。若继续该路线，下一最小诊断应在reserve内部实际申请前后区分malloc与注册返回值，并记录chunk/组合caps的free和largest；这是另一次经确认的诊断，不在本轮日志分析中修改。恢复配置亦尚未执行，源码当前仍为229376。

## A版region来源证据

配置A为CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=200000。依据不仅是长度相近：

1. IDF5.5.5 `components/freertos/app_startup.c:174-176` 在回收启动栈后、app_main之前，将该配置传入esp_psram_extram_reserve_dma_pool。
2. 当前A版ELF反汇编在0x4820dc00加载0x30d40（200000）到a0，0x4820dc08调用0x4820ce0a的reserve函数。证据保存在logs/reserve_internal/A/reserve_call.txt；map对应esp_psram库esp_psram.c.obj。
3. `components/esp_psram/system_layer/esp_psram.c:635-660`：按DMA|INTERNAL最大块分批heap_caps_malloc，再以[start,start+next_size-1]调用heap_caps_add_region_with_caps。
4. `components/heap/heap_caps_init.c`保存start/end，multi_heap_register(start,end-start)，故200000B申请对应summary的199999长度。允许在已分配父heap内部注册嵌套heap。
5. A运行时region起点0x4ff861c0、注册终点0x4ffb6eff，差199999；在父heap覆盖范围内，region末端与最大free末端0x4ffb6ef8相邻。项目main/components没有另一个heap_caps_add_region调用。

以上启动调用、二进制参数、注册算式、map与嵌套运行布局共同支持该region就是reserve子堆。尚未取得调试器直接观察返回dma_heap指针的记录，不能把map说成动态heap地址的直接证明。

## 能力与分配规则

reserve注册caps优先级数组为：`{0, DMA|INTERNAL, DEFAULT|8BIT|32BIT}`。并非H264专用池，也不是永久不可使用的guard。

显式DMA/Internal申请可以进入；FreeRTOS TCB、普通内部栈、硬件DMA对象都会使用。普通malloc按heap_caps.c:109起的策略选DEFAULT|INTERNAL或DEFAULT|SPIRAM，在其他候选不足时也可使用这个低优先级DEFAULT池。小对象仍可切割它。

reserve并不增加RAM。扩大29376B（28.6875KiB），通常意味着父普通heap可用空间相应减少，并改变分配落点。实现明确允许拆成多个不连续chunk；229376不是单一连续块保证，不能推算largest必然增加29376。

## B配置及实验边界

唯一配置变量变更：200000 → 229376（224KiB）。同步到src/demo/sdkconfig、build_rtc_mem_b/sdkconfig、sdkconfig.defaults，避免两个构建目录和默认配置不同。本轮未修改C/C++业务或诊断源码、task、queue、DMA和音视频参数。

A配置/ELF/map保存于logs/reserve_internal/A。按用户要求fullclean后build。fullclean组件管理扩展触发依赖清理并报告本地修改；保留修改过的组件，改用禁用组件管理扩展的构建目录清理，随后按锁定依赖恢复缺失的未修改组件。手动日志均在logs/reserve_internal。

## 当前A/B结果（不能以构建代替实测）

| 项目 | A=200000实测 | B=229376 |
|---|---|---|
| reserve region | 0x4ff861c0..0x4ffb6eff，len199999 | 待冷启动实测；单chunk预期len229375，可能拆分 |
| H264前INT free/largest | 209803 / 77824 | 待实测 |
| H264前DMA free/largest | 174803 / 77824 | 待实测 |
| H264 REF | NO_MEM，ptr0，actual92224 | 待实测 |
| USB/Hosted/I2S/LCD/SD/AFE | A日志可见各初始化/运行，不能替代B验证 | 待实测 |
| 60秒、10轮、语音恢复 | 未达到本轮验收 | 待H264成功后验证 |

当前仅枚举到COM3/COM4蓝牙串口，无P4 COM7，不能执行烧录和实机测试。没有宣称H264成功或推荐正式保留229376。

用户随后明确选择自行烧录，本轮不操作串口。两个构建目录全量重建均已成功；RTC第一次静态库打包报目标文件缺失（文件实际存在），重跑剩余构建通过，未改源码修复。B版ELF反汇编确认reserve调用参数0x38000=229376，两个生成sdkconfig.h均为229376。配置逐项对照仅一个值变化，见config_diff.txt；新ELF/BIN哈希见logs/reserve_internal/B_manifest.json。实机结果仍全部待测。

## 现有观测限制

FRAG定向筛选硬编码旧起点0x4ff861c0或长度199999；B移动或改变长度后可能found=0。按单变量要求本轮不改此诊断。已有heap summary仅在H264失败路径打印，成功时未必输出新region完整边界。启动的“Reserving pool 224K”只证明请求，不能单独证明单region229375。

因此第一次B日志需保留完整冷启动到RTC停止。如果现有日志不足以核对region边界，应使用外部调试器读取registered_heaps；如工具不可用，先报告证据缺失，再另行约定A/B一致的只读观测手段，不悄悄增加第二个配置变量。

## 验收与失败决策

B必须同时验证：BOOT/WAKE_IDLE/RTC_START/AFE释放后/H264_OPEN_BEFORE的free和largest、REF申请OK非空；USB/UVC、Hosted、音频、LCD、SD正常，无新NO_MEM/WDT。一次H264成功只算第一关。

随后720p20fps连续60秒，STOP释放REF并恢复AFE/Wake和实际语音，START/STOP十轮记录free/largest、PSRAM、REF ptr、最低值；不能仅靠“restore=OK”替代语音验证。

若B失败，先比较子region增长、largest增长、父heap缩减和失败模块；若reserve拆分或新增空间被别的长期对象消耗，不继续机械增加。232/240KiB仅在证据支持且用户确认后作为下一次独立配置实验。

## 独立早创建方案：只评估

当前WAKE_IDLE附近INT约136KB、DMA约101KB。若粗略扣除92224B长期REF，剩余约44KB INT、9KB DMA，且编码器还有其他对象、对齐及瞬时峰值，风险很高。这不是精确模拟，因为早创建会改变布局，但足以否定“长期持有一定安全”。

Hosted、USB、I2S、LCD、SD、AFE都需要运行余量。历史96KB guard曾造成DMA压力，真实REF也消耗相同类型物理内存，不能只因它有业务用途就忽略预算。暂不实施早创建/长期持有；先完成单变量reserve实验。
