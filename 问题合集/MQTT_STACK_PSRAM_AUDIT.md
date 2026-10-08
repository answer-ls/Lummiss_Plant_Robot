# MQTT 6KiB任务栈PSRAM审核

## 结论

当前MQTT栈是长期6144B INTERNAL，可作为单独PSRAM实验候选，但尚未完成实机TLS/重连验证，不能标为已证明安全。本轮只恢复reserve=200000、编译并审核，不迁移MQTT栈、不改esp-mqtt或TLS源码。

该6KiB allocation位于早期FRAG的0x4ff88xxx区域，不在当前关键52块链（0x4ff9f654..0x4ffa3e64）中。迁移可能改变启动分配顺序、改善总量，但不能按6144B直接相加到77824的largest。TCB仍需INTERNAL。

## 创建与销毁

- 项目components/cloud_mqtt/cloud_mqtt.c:31定义CLOUD_MQTT_TASK_STACK=6144，约503行配置.task.stack_size；没有stack caps字段。
- IDF components/mqtt/esp-mqtt/mqtt_client.c:1855起esp_mqtt_client_start使用xTaskCreate或xTaskCreatePinnedToCore，当前CONFIG_MQTT_TASK_CORE_SELECTION_ENABLED未启用，走1876行普通xTaskCreate，因此动态内部栈。
- include/mqtt_client.h的task配置只给priority/stack_size；Kconfig未提供PSRAM栈开关。CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC只改变TLS堆分配，不会迁移MQTT任务栈。
- mqtt_client.c:1852退出时vTaskDelete(NULL)。若以后改为WithCaps创建，必须同步使用vTaskDeleteWithCaps清理独立栈/TCB，不能仅替换创建API。保留原stop/destroy握手和原核/优先级/6144B大小，检查任务创建失败与重启路径。

## MQTT任务实际执行路径

esp_mqtt_task → transport connect/read/write → esp_tls/mbedtls → lwIP/socket。

MQTT私有事件循环task_name=NULL（mqtt_client.c:901），由任务调用esp_event_loop_run（1069/1644），所以项目mqtt_event_handler确实在此任务上下文执行，并非自动移到另一个事件任务。

| 回调分支 | 当前操作 | 关闭cache/Flash风险审核 |
|---|---|---|
| CONNECTED/SUBSCRIBED | subscribe、发布hello | 同步协议/TLS发送，需TLS实测 |
| hello | JSON、拷贝session、状态及日志 | 已读路径未见Flash写/模型销毁 |
| mcp | cloud_mcp_submit拷贝并入队 | 工具执行在cloud_mcp任务，不在MQTT回调 |
| tts/stt/listen/goodbye | server_text_callback的CLOUD_PROTOCOL_V3分支，voice event入队 | AFE/会话操作由消费者处理，回调不调用srmodel_deinit/munmap |
| llm | expression_manager事件投递 | 不直接执行UI渲染或模型加载 |
| ERROR/DISCONNECTED | 记录错误、清理会话状态 | 未见NVS/Flash写 |

## TLS与cache边界

cloud_mqtt_start约488行使用esp_crt_bundle_attach做服务器认证；未配置客户端私钥、DS或secure element。esp_tls_mbedtls.c存在DS/NVS相关可选路径，但不是当前配置启用的凭据路径，不能仅因源码出现NVS就断定MQTT栈不可迁。

当前CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y；硬件SHA/MPI/ECC等开启。这说明必须审查加速器内部缓冲要求，不能根据“TLS数据在PSRAM”推出“调用栈一定可PSRAM”。当前应用回调和TLS接入代码未发现直接cache关闭调用，但这不是所有库/ROM可达路径的形式化证明，仍需握手、认证失败、重连和并行系统操作的实机A/B。

CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y仅放开任务创建，不解除cache-disabled时PSRAM不可访问的限制。wake_cleanup必须INTERNAL的既有结论不变。

进一步核对IDF mbedtls/port/sha/core/sha.c:323起esp_sha_dma：非DMA input调用esp_sha_block_mode_fallback；非DMA局部buf申请INTERNAL|DMA|8BIT中转并在cleanup释放。因此不能因开启硬件SHA就认定整个MQTT任务栈必须INTERNAL；迁栈也不会消除TLS少量临时DMA需求。其余硬件加速/库路径尚无PSRAM栈实机覆盖记录。

## 若批准下一次实验，最小patch范围

只针对esp-mqtt任务的创建与自删除配对，使用WithCaps SPIRAM|8BIT；不动MQTT buffer/TLS heap/优先级/核/协议/重连策略。应保留为可追踪的项目组件补丁，避免影响其他项目共用SDK；不使用全局xTaskCreate拦截或顺带迁移所有任务。

失败创建应返回原错误，不能悄悄回退INTERNAL掩盖实验结果。验证成功连接、收发和hello、MCP/语音入队、多轮断开重连、stop/destroy再start，无cache assert/栈溢出/泄漏；记录实际栈地址和high-water、INTERNAL free/largest、DMA free/largest，并独立验证H264。节省预期为约6KiB内部栈payload，不包含TCB，也不保证形成大连续块。

## 本轮恢复

src/demo/sdkconfig、src/demo/build_rtc_mem_b/sdkconfig、src/demo/sdkconfig.defaults均从229376恢复200000。使用idf.py build增量构建，不再次fullclean；配置变化可触发较多组件重编译，这是正常依赖行为。构建日志在logs/reserve_internal/restore_*.log。不主动烧录。

两个构建均成功，生成sdkconfig.h均确认200000；有效配置项逐项对照A备份完全一致。固件哈希保存于logs/reserve_internal/restore_manifest.json。尚未烧录，恢复版实际冷启动待用户验证。
