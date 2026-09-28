# 分配调用点检索清单

扫描 3237 个源码文件，命中 1365 个调用/声明候选。包含当前编译单元及项目组件源码；编译标记不能排除 #if 未启用分支，宏、封装和预编译库需结合人工审计。函数名是词法提示，不是AST分析。

| 文件:行 | 函数提示 | 当前编译单元 | 调用表达式 |
|---|---|---|---|
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/app_update/esp_ota_ops.c:125 | esp_ota_init_entry | True | `calloc(1, sizeof(ota_ops_entry_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bootloader_support/src/bootloader_sha.c:187 | bootloader_sha256_start | True | `malloc(sizeof(mbedtls_sha256_context))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bootloader_support/src/bootloader_sha.c:225 | bootloader_sha512_start | True | `malloc(sizeof(mbedtls_sha512_context))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/common/ble_log/deprecated/ble_log_spi_out.c:42 | ? | True | `heap_caps_malloc(size, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/common/ble_log/deprecated/ble_log_spi_out.c:1047 | 见源码上下文 | True | `xTaskCreate(spi_out_task, "BLELogSPIOut", SPI_OUT_TASK_STACK_SIZE, NULL, SPI_OUT_TASK_PRIORITY, &spi_out_task_handle)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/common/btc/profile/esp/blufi/nimble_host/esp_blufi.c:137 | 见源码上下文 | True | `malloc(pkt_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/common/hci_log/bt_hci_log.c:58 | bt_hci_log_init | True | `malloc(BT_HCI_LOG_DATA_BUF_SIZE)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/common/hci_log/bt_hci_log.c:62 | 见源码上下文 | True | `malloc(BT_HCI_LOG_ADV_BUF_SIZE)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/common/hci_log/bt_hci_log.c:271 | 见源码上下文 | True | `malloc((size_t)record_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/common/osi/allocator.c:257 | 见源码上下文 | True | `heap_caps_get_allocated_size(p)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/common/osi/allocator.c:282 | 见源码上下文 | True | `heap_caps_get_allocated_size(p)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/common/osi/allocator.c:300 | 见源码上下文 | True | `heap_caps_get_allocated_size(ptr)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/common/osi/thread.c:76 | 见源码上下文 | True | `xQueueCreate(capacity, sizeof(struct work_item))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/common/osi/thread.c:261 | 见源码上下文 | True | `xTaskCreatePinnedToCore(osi_thread_run, name, stack_size, &start_arg, priority, &thread->thread_handle, core)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/nimble/nimble/host/services/prox/src/ble_svc_prox.c:361 | MYNEWT_VAL | True | `xTaskCreate(ble_prox_prph_task, "ble_prox_prph_task", 4096, NULL, 10, &ble_prox_task_handle)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/nimble/porting/npl/freertos/src/nimble_port_freertos.c:43 | esp_nimble_enable | True | `xTaskCreatePinnedToCore(host_task, "nimble_host", NIMBLE_HS_STACK_SIZE, NULL, (configMAX_PRIORITIES - 4), &host_task_h, NIMBLE_CORE)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/nimble/porting/npl/freertos/src/npl_os_freertos.c:305 | 见源码上下文 | True | `xQueueCreate(BLE_TOTAL_EV_COUNT, sizeof(struct ble_npl_eventq *))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/nimble/porting/npl/freertos/src/npl_os_freertos.c:315 | 见源码上下文 | True | `xQueueCreate(BLE_TOTAL_EV_COUNT, sizeof(struct ble_npl_eventq *))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/port/src/esp_nimble_mem.c:198 | nimble_mem_dbg_realloc | True | `realloc(ptr, new_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/port/src/esp_nimble_mem.c:262 | nimble_mem_malloc | True | `heap_caps_malloc(size, MALLOC_CAP_INTERNAL&#124;MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/port/src/esp_nimble_mem.c:264 | nimble_mem_malloc | True | `heap_caps_malloc(size, MALLOC_CAP_SPIRAM&#124;MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/port/src/esp_nimble_mem.c:266 | nimble_mem_malloc | True | `heap_caps_malloc_prefer(size, 2, MALLOC_CAP_INTERNAL&#124;MALLOC_CAP_IRAM_8BIT, MALLOC_CAP_INTERNAL&#124;MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/port/src/esp_nimble_mem.c:268 | nimble_mem_malloc | True | `malloc(size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/port/src/esp_nimble_mem.c:280 | 见源码上下文 | True | `heap_caps_get_allocated_size(mem)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/port/src/esp_nimble_mem.c:293 | nimble_mem_calloc | True | `heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL&#124;MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/port/src/esp_nimble_mem.c:295 | nimble_mem_calloc | True | `heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM&#124;MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/port/src/esp_nimble_mem.c:297 | nimble_mem_calloc | True | `heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_INTERNAL&#124;MALLOC_CAP_IRAM_8BIT, MALLOC_CAP_INTERNAL&#124;MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/port/src/esp_nimble_mem.c:299 | nimble_mem_calloc | True | `calloc(n, size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/port/src/esp_nimble_mem.c:303 | 见源码上下文 | True | `heap_caps_get_allocated_size(mem)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/bt/host/nimble/port/src/esp_nimble_mem.c:316 | 见源码上下文 | True | `heap_caps_get_allocated_size(ptr)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/argtable3/arg_utils.c:88 | xmalloc | True | `malloc(size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/argtable3/arg_utils.c:98 | xcalloc | True | `calloc(allocated_count, allocated_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/argtable3/arg_utils.c:107 | xrealloc | True | `realloc(ptr, allocated_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/commands.c:74 | 见源码上下文 | True | `heap_caps_calloc(1, config->max_cmdline_length, s_config.heap_alloc_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/commands.c:130 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(*item), s_config.heap_alloc_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/commands.c:239 | 见源码上下文 | True | `heap_caps_calloc(s_config.max_cmdline_args, sizeof(char *), s_config.heap_alloc_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/esp_console_repl_chip.c:50 | 见源码上下文 | True | `calloc(1, sizeof(esp_console_repl_universal_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/esp_console_repl_chip.c:86 | 见源码上下文 | True | `xTaskCreatePinnedToCore(esp_console_repl_task, "console_repl", repl_config->task_stack_size, cdc_repl, repl_config->task_priority, &cdc_repl->repl_com.task_hdl, repl_config->task_core_id)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/esp_console_repl_chip.c:116 | 见源码上下文 | True | `calloc(1, sizeof(esp_console_repl_universal_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/esp_console_repl_chip.c:163 | 见源码上下文 | True | `xTaskCreatePinnedToCore(esp_console_repl_task, "console_repl", repl_config->task_stack_size, usb_serial_jtag_repl, repl_config->task_priority, &usb_serial_jtag_repl->repl_com.task_hdl, repl_config->task_core_id)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/esp_console_repl_chip.c:193 | 见源码上下文 | True | `calloc(1, sizeof(esp_console_repl_universal_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/esp_console_repl_chip.c:300 | 见源码上下文 | True | `xTaskCreatePinnedToCore(esp_console_repl_task, "console_repl", repl_config->task_stack_size, uart_repl, repl_config->task_priority, &uart_repl->repl_com.task_hdl, repl_config->task_core_id)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/linenoise/linenoise.c:497 | linenoiseAddCompletion | True | `malloc(len+1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/linenoise/linenoise.c:500 | linenoiseAddCompletion | True | `realloc(lc->cvec,sizeof(char*)*(lc->len+1))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/linenoise/linenoise.c:526 | abAppend | True | `realloc(ab->b,ab->len+len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/linenoise/linenoise.c:1227 | linenoise | True | `calloc(1, max_cmdline_length)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/linenoise/linenoise.c:1284 | 见源码上下文 | True | `malloc(sizeof(char*)*history_max_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/linenoise/linenoise.c:1317 | 见源码上下文 | True | `malloc(sizeof(char*)*len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/console/linenoise/linenoise.c:1363 | 见源码上下文 | True | `calloc(1, max_cmdline_length)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/adc_dma_legacy.c:236 | adc_digi_initialize | True | `calloc(1, sizeof(adc_digi_context_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/adc_dma_legacy.c:243 | 见源码上下文 | True | `xRingbufferCreate(init_config->max_store_buf_size, RINGBUF_TYPE_BYTEBUF)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/adc_dma_legacy.c:250 | 见源码上下文 | True | `heap_caps_calloc(1, init_config->conv_num_each_intr * INTERNAL_BUF_NUM, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/adc_dma_legacy.c:259 | 见源码上下文 | True | `heap_caps_calloc(1, (sizeof(dma_descriptor_t)) * dma_desc_max_num, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/adc_dma_legacy.c:266 | 见源码上下文 | True | `calloc(1, SOC_ADC_PATT_LEN_MAX * sizeof(adc_digi_pattern_config_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/i2s_legacy.c:585 | i2s_alloc_dma_buffer | True | `heap_caps_aligned_calloc(4, 1, sizeof(char) * dma_obj->buf_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/i2s_legacy.c:593 | i2s_alloc_dma_buffer | True | `heap_caps_aligned_calloc(4, 1, sizeof(lldesc_t), MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/i2s_legacy.c:671 | i2s_create_dma_object | True | `calloc(1, sizeof(i2s_dma_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/i2s_legacy.c:674 | i2s_create_dma_object | True | `heap_caps_calloc(buf_cnt, sizeof(char *), MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/i2s_legacy.c:679 | 见源码上下文 | True | `heap_caps_calloc(buf_cnt, sizeof(lldesc_t *), MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/i2s_legacy.c:684 | 见源码上下文 | True | `xQueueCreate(buf_cnt - 1, sizeof(char *))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/i2s_legacy.c:1681 | i2s_driver_install | True | `calloc(1, sizeof(i2s_obj_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/i2s_legacy.c:1703 | 见源码上下文 | True | `xQueueCreate(queue_size, sizeof(i2s_event_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/pcnt_legacy.c:329 | _pcnt_isr_service_install | True | `calloc(SOC_PCNT_UNITS_PER_GROUP, sizeof(pcnt_isr_func_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/pcnt_legacy.c:410 | pcnt_init | True | `heap_caps_calloc(1, sizeof(pcnt_obj_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/rmt_legacy.c:1033 | 见源码上下文 | True | `calloc(1, sizeof(rmt_obj_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/rmt_legacy.c:1036 | 见源码上下文 | True | `calloc(1, sizeof(rmt_obj_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/rmt_legacy.c:1038 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(rmt_obj_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/rmt_legacy.c:1070 | 见源码上下文 | True | `xRingbufferCreate(rx_buf_size, RINGBUF_TYPE_NOSPLIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/rmt_legacy.c:1077 | 见源码上下文 | True | `calloc(1, rx_buf_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/rmt_legacy.c:1080 | 见源码上下文 | True | `calloc(1, rx_buf_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/rmt_legacy.c:1082 | 见源码上下文 | True | `heap_caps_calloc(1, rx_buf_size, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/rmt_legacy.c:1219 | 见源码上下文 | True | `calloc(1, block_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/rmt_legacy.c:1222 | 见源码上下文 | True | `heap_caps_calloc(1, block_size, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/rmt_legacy.c:1224 | 见源码上下文 | True | `calloc(1, block_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/sigma_delta_legacy.c:85 | sigmadelta_init | True | `heap_caps_calloc(1, sizeof(sigmadelta_obj_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/deprecated/timer_legacy.c:311 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(timer_obj_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/i2c/i2c.c:320 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(i2c_obj_t), alloc_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/i2c/i2c.c:348 | 见源码上下文 | True | `xRingbufferCreate(slv_rx_buf_len, RINGBUF_TYPE_BYTEBUF)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/i2c/i2c.c:359 | 见源码上下文 | True | `xRingbufferCreate(slv_tx_buf_len, RINGBUF_TYPE_BYTEBUF)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/i2c/i2c.c:392 | 见源码上下文 | True | `xQueueCreateWithCaps(I2C_EVT_QUEUE_LEN, sizeof(i2c_cmd_evt_t), alloc_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/i2c/i2c.c:1189 | i2c_cmd_link_create | True | `heap_caps_calloc(1, sizeof(i2c_cmd_desc_t), alloc_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/i2c/i2c.c:1251 | 见源码上下文 | True | `heap_caps_calloc(n, size, alloc_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/twai/twai.c:400 | twai_alloc_driver_obj | True | `heap_caps_calloc(1, sizeof(twai_obj_t) + twai_hal_get_mem_requirment(), TWAI_MALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/twai/twai.c:405 | 见源码上下文 | True | `xQueueCreateWithCaps(g_config->tx_queue_len, sizeof(twai_hal_frame_t), TWAI_MALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/driver/twai/twai.c:407 | 见源码上下文 | True | `xQueueCreateWithCaps(g_config->rx_queue_len, sizeof(twai_hal_frame_t), TWAI_MALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp-tls/esp_tls.c:195 | esp_tls_init | True | `calloc(1, sizeof(esp_tls_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp-tls/esp_tls.c:724 | defined | True | `calloc(1, sizeof(esp_tls_server_session_ticket_ctx_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp-tls/esp_tls_error_capture.c:42 | esp_tls_internal_event_tracker_create | True | `calloc(1, sizeof(struct esp_tls_error_storage))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp-tls/esp_tls_mbedtls.c:233 | 见源码上下文 | True | `calloc(1, sizeof(esp_tls_client_session_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp-tls/esp_tls_mbedtls.c:250 | 见源码上下文 | True | `calloc(1, sizeof(esp_tls_client_session_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp-tls/esp_tls_mbedtls.c:347 | 见源码上下文 | True | `calloc(1, sizeof(esp_tls_client_session_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp-tls/esp_tls_mbedtls.c:377 | 见源码上下文 | True | `calloc(1, session_ticket_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp-tls/esp_tls_mbedtls.c:1221 | 见源码上下文 | True | `calloc(1, sizeof(mbedtls_x509_crt))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp-tls/esp_tls_mbedtls.c:1373 | esp_mbedtls_init_pk_ctx_for_ds | True | `calloc(1, sizeof(mbedtls_rsa_context))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_adc/adc_cali_curve_fitting.c:82 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(adc_cali_scheme_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_adc/adc_cali_curve_fitting.c:85 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(cali_chars_curve_fitting_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_adc/adc_continuous.c:169 | adc_continuous_new_handle | True | `heap_caps_calloc(1, sizeof(adc_continuous_ctx_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_adc/adc_continuous.c:177 | 见源码上下文 | True | `heap_caps_calloc(1, hdl_config->max_store_buf_size, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_adc/adc_continuous.c:178 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(StaticRingbuffer_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_adc/adc_continuous.c:185 | 见源码上下文 | True | `xRingbufferCreateStatic(hdl_config->max_store_buf_size, RINGBUF_TYPE_BYTEBUF, adc_ctx->ringbuf_storage, adc_ctx->ringbuf_struct)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_adc/adc_continuous.c:192 | 见源码上下文 | True | `heap_caps_calloc(INTERNAL_BUF_NUM, hdl_config->conv_frame_size, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_DMA &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_adc/adc_continuous.c:201 | 见源码上下文 | True | `heap_caps_aligned_calloc(ADC_DMA_DESC_ALIGN, dma_desc_max_num, sizeof(dma_descriptor_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_DMA &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_adc/adc_continuous.c:215 | 见源码上下文 | True | `calloc(1, SOC_ADC_PATT_LEN_MAX * sizeof(adc_digi_pattern_config_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_adc/adc_continuous.c:648 | adc_continuous_read_parse | True | `malloc(raw_buffer_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_adc/adc_oneshot.c:95 | adc_oneshot_new_unit | True | `heap_caps_calloc(1, sizeof(adc_oneshot_unit_ctx_t), ADC_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ana_cmpr/ana_cmpr.c:114 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(struct ana_cmpr_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ana_cmpr/ana_cmpr_etm.c:29 | ana_cmpr_new_etm_event | True | `heap_caps_calloc(1, sizeof(ana_cmpr_etm_event_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_bitscrambler/src/bitscrambler.c:162 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(bitscrambler_t), BITSCRAMBLER_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_bitscrambler/src/bitscrambler_loopback.c:80 | 见源码上下文 | True | `calloc(1, sizeof(bitscrambler_loopback_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_gpio/src/dedic_gpio.c:79 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(dedic_gpio_platform_t), DEDIC_GPIO_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_gpio/src/dedic_gpio.c:217 | dedic_gpio_new_bundle | True | `heap_caps_calloc(1, bundle_size, DEDIC_GPIO_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_gpio/src/gpio.c:537 | gpio_install_isr_service | True | `heap_caps_calloc(GPIO_NUM_MAX, sizeof(gpio_isr_func_t), alloc_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_gpio/src/gpio_etm.c:175 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(gpio_etm_event_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_gpio/src/gpio_etm.c:267 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(gpio_etm_task_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_gpio/src/gpio_flex_glitch_filter.c:131 | gpio_new_flex_glitch_filter | True | `heap_caps_calloc(1, sizeof(gpio_flex_glitch_filter_t), FILTER_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_gpio/src/gpio_pin_glitch_filter.c:84 | gpio_new_pin_glitch_filter | True | `heap_caps_calloc(1, sizeof(gpio_pin_glitch_filter_t), FILTER_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_gptimer/src/gptimer.c:144 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(gptimer_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_gptimer/src/gptimer.c:180 | 见源码上下文 | True | `heap_caps_get_allocated_size(timer)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_gptimer/src/gptimer_common.c:30 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(gptimer_group_t), GPTIMER_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_gptimer/src/gptimer_etm.c:32 | gptimer_new_etm_event | True | `heap_caps_calloc(1, sizeof(esp_etm_event_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_gptimer/src/gptimer_etm.c:54 | gptimer_new_etm_task | True | `heap_caps_calloc(1, sizeof(esp_etm_task_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2c/i2c_common.c:93 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(i2c_bus_t), I2C_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2c/i2c_master.c:1054 | i2c_new_master_bus | True | `heap_caps_calloc(1, sizeof(i2c_master_bus_t) + 20 * sizeof(i2c_transaction_t), I2C_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2c/i2c_master.c:1104 | LP_I2C_SRC_CLK_ATOMIC | True | `xQueueCreateWithCaps(1, sizeof(i2c_master_event_t), I2C_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2c/i2c_master.c:1132 | 见源码上下文 | True | `heap_caps_calloc(bus_config->trans_queue_depth * I2C_TRANS_QUEUE_MAX, sizeof(i2c_transaction_t), I2C_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2c/i2c_master.c:1136 | 见源码上下文 | True | `xQueueCreateWithCaps(bus_config->trans_queue_depth, sizeof(i2c_transaction_t), I2C_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2c/i2c_master.c:1149 | 见源码上下文 | True | `heap_caps_calloc(bus_config->trans_queue_depth, sizeof(*i2c_master->i2c_async_ops), I2C_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2c/i2c_master.c:1188 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(i2c_master_dev_t), I2C_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2c/i2c_master.c:1197 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(i2c_master_device_list_t), I2C_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2c/i2c_slave.c:201 | i2c_new_slave_device | True | `heap_caps_calloc(1, sizeof(i2c_slave_dev_t), I2C_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2c/i2c_slave.c:222 | 见源码上下文 | True | `xRingbufferCreateWithCaps(slave_config->send_buf_depth, RINGBUF_TYPE_BYTEBUF, I2C_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2c/i2c_slave.c:229 | 见源码上下文 | True | `xQueueCreateWithCaps(1, sizeof(i2c_slave_evt_t), I2C_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_common.c:83 | __attribute__ | True | `heap_caps_aligned_calloc(4, num, size, I2S_DMA_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_common.c:239 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(i2s_controller_t), I2S_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_common.c:315 | i2s_register_channel | True | `heap_caps_calloc(1, sizeof(struct i2s_channel_obj_t), I2S_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_common.c:329 | i2s_register_channel | True | `xQueueCreateWithCaps(desc_num - 1, sizeof(uint8_t *), I2S_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_common.c:526 | i2s_alloc_dma_desc | True | `heap_caps_calloc(num, sizeof(lldesc_t *), I2S_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_common.c:528 | i2s_alloc_dma_desc | True | `heap_caps_calloc(num, sizeof(uint8_t *), I2S_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_etm.c:55 | i2s_new_etm_event | True | `heap_caps_calloc(1, sizeof(esp_etm_event_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_etm.c:87 | i2s_new_etm_task | True | `heap_caps_calloc(1, sizeof(i2s_etm_task_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_pdm.c:215 | 见源码上下文 | True | `calloc(1, sizeof(i2s_pdm_tx_config_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_pdm.c:548 | 见源码上下文 | True | `calloc(1, sizeof(i2s_pdm_rx_config_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_std.c:318 | 见源码上下文 | True | `calloc(1, sizeof(i2s_std_config_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/i2s_tdm.c:304 | 见源码上下文 | True | `calloc(1, sizeof(i2s_tdm_config_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/lp_i2s.c:61 | lp_i2s_new_channel | True | `heap_caps_calloc(1, sizeof(lp_i2s_controller_t), LP_I2S_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/lp_i2s.c:282 | s_i2s_register_channel | True | `heap_caps_calloc(1, sizeof(struct lp_i2s_channel_obj_t), LP_I2S_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_i2s/lp_i2s_vad.c:58 | lp_i2s_vad_new_unit | True | `heap_caps_calloc(1, sizeof(vad_unit_ctx_t), LP_VAD_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_common.c:46 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(jpeg_codec_t), JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_common.c:143 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(jpeg_isr_handler_t), JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:74 | jpeg_new_decoder_engine | True | `heap_caps_calloc(1, sizeof(jpeg_decoder_t), JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:81 | jpeg_new_decoder_engine | True | `heap_caps_aligned_calloc(alignment, 1, dma_desc_mem_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:83 | jpeg_new_decoder_engine | True | `heap_caps_aligned_calloc(alignment, 1, dma_desc_mem_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:87 | jpeg_new_decoder_engine | True | `heap_caps_calloc(1, sizeof(jpeg_dec_header_info_t), JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:112 | 见源码上下文 | True | `xQueueCreateWithCaps(2, sizeof(jpeg_dma2d_dec_evt_t), JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:121 | 见源码上下文 | True | `heap_caps_calloc(1, SIZEOF_DMA2D_TRANS_T, JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:140 | jpeg_decoder_get_info | True | `heap_caps_calloc(1, sizeof(jpeg_dec_header_info_t), JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:399 | 见源码上下文 | True | `heap_caps_aligned_calloc(cache_align, 1, size, MALLOC_CAP_SPIRAM)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_decode.c:402 | 见源码上下文 | True | `heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_encode.c:111 | jpeg_new_encoder_engine | True | `heap_caps_calloc(1, sizeof(jpeg_encoder_t), MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_encode.c:118 | jpeg_new_encoder_engine | True | `heap_caps_aligned_calloc(alignment, 1, dma_desc_mem_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_encode.c:120 | jpeg_new_encoder_engine | True | `heap_caps_aligned_calloc(alignment, 1, dma_desc_mem_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_encode.c:126 | jpeg_new_encoder_engine | True | `xQueueCreateWithCaps(2, sizeof(jpeg_enc_dma2d_evt_t), JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_encode.c:149 | 见源码上下文 | True | `heap_caps_calloc(1, SIZEOF_DMA2D_TRANS_T, JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_encode.c:152 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(jpeg_enc_header_info_t), JPEG_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_encode.c:382 | 见源码上下文 | True | `heap_caps_aligned_calloc(cache_align, 1, size, MALLOC_CAP_SPIRAM)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_jpeg/jpeg_encode.c:385 | 见源码上下文 | True | `heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ledc/src/ledc.c:417 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(ledc_obj_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ledc/src/ledc.c:1315 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(ledc_fade_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_cap.c:79 | mcpwm_new_capture_timer | True | `heap_caps_calloc(1, sizeof(mcpwm_cap_timer_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_cap.c:264 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(mcpwm_cap_channel_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_cmpr.c:105 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(mcpwm_oper_cmpr_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_cmpr.c:168 | mcpwm_new_event_comparator | True | `heap_caps_calloc(1, sizeof(mcpwm_evt_cmpr_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_com.c:45 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(mcpwm_group_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_etm.c:69 | mcpwm_comparator_new_etm_event | True | `heap_caps_calloc(1, sizeof(mcpwm_comparator_etm_event_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_etm.c:121 | mcpwm_timer_new_etm_event | True | `heap_caps_calloc(1, sizeof(mcpwm_timer_etm_event_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_fault.c:81 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(mcpwm_gpio_fault_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_fault.c:171 | mcpwm_new_soft_fault | True | `heap_caps_calloc(1, sizeof(mcpwm_soft_fault_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_gen.c:58 | mcpwm_new_generator | True | `heap_caps_calloc(1, sizeof(mcpwm_gen_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_oper.c:76 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(mcpwm_oper_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_sync.c:56 | mcpwm_new_timer_sync_src | True | `heap_caps_calloc(1, sizeof(mcpwm_timer_sync_src_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_sync.c:165 | mcpwm_new_gpio_sync_src | True | `heap_caps_calloc(1, sizeof(mcpwm_gpio_sync_src_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_sync.c:235 | mcpwm_new_soft_sync_src | True | `heap_caps_calloc(1, sizeof(mcpwm_soft_sync_src_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_mcpwm/src/mcpwm_timer.c:88 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(mcpwm_timer_t), MCPWM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_parlio/src/parlio_common.c:28 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(parlio_group_t), PARLIO_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_parlio/src/parlio_rx.c:645 | parlio_new_rx_unit | True | `heap_caps_calloc(1, sizeof(parlio_rx_unit_t), PARLIO_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_parlio/src/parlio_rx.c:661 | parlio_new_rx_unit | True | `xQueueCreateWithCaps(config->trans_queue_depth, sizeof(parlio_rx_transaction_t), PARLIO_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_parlio/src/parlio_rx.c:682 | 见源码上下文 | True | `heap_caps_aligned_calloc(max_alignment, 2, max_alignment, PARLIO_MEM_ALLOC_CAPS &#124; MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_parlio/src/parlio_rx.c:775 | 见源码上下文 | True | `heap_caps_aligned_calloc(rx_unit->int_mem_align, 1, trans.aligned_payload.buf.body.length, PARLIO_DMA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_parlio/src/parlio_rx.c:853 | parlio_new_rx_level_delimiter | True | `heap_caps_calloc(1, sizeof(parlio_rx_delimiter_t), PARLIO_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_parlio/src/parlio_rx.c:887 | parlio_new_rx_pulse_delimiter | True | `heap_caps_calloc(1, sizeof(parlio_rx_delimiter_t), PARLIO_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_parlio/src/parlio_rx.c:918 | parlio_new_rx_soft_delimiter | True | `heap_caps_calloc(1, sizeof(parlio_rx_delimiter_t), PARLIO_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_parlio/src/parlio_rx.c:1026 | 见源码上下文 | True | `heap_caps_aligned_calloc(alignment, 1, payload_size, PARLIO_DMA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_parlio/src/parlio_tx.c:25 | parlio_tx_create_trans_queue | True | `xQueueCreateWithCaps(config->trans_queue_depth, sizeof(parlio_tx_trans_desc_t *), PARLIO_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_parlio/src/parlio_tx.c:301 | parlio_new_tx_unit | True | `heap_caps_calloc(1, sizeof(parlio_tx_unit_t) + sizeof(parlio_tx_trans_desc_t) * config->trans_queue_depth, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_pcnt/src/pulse_cnt.c:231 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(pcnt_unit_t), PCNT_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_pcnt/src/pulse_cnt.c:810 | pcnt_new_channel | True | `heap_caps_calloc(1, sizeof(pcnt_chan_t), PCNT_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_pcnt/src/pulse_cnt.c:953 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(pcnt_group_t), PCNT_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ppa/src/ppa_core.c:79 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(ppa_srm_engine_t), PPA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ppa/src/ppa_core.c:81 | 见源码上下文 | True | `heap_caps_aligned_calloc(alignment, 1, s_platform.dma_desc_mem_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ppa/src/ppa_core.c:82 | 见源码上下文 | True | `heap_caps_aligned_calloc(alignment, 1, s_platform.dma_desc_mem_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ppa/src/ppa_core.c:124 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(ppa_blend_engine_t), PPA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ppa/src/ppa_core.c:126 | 见源码上下文 | True | `heap_caps_aligned_calloc(alignment, 1, s_platform.dma_desc_mem_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ppa/src/ppa_core.c:127 | 见源码上下文 | True | `heap_caps_aligned_calloc(alignment, 1, s_platform.dma_desc_mem_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ppa/src/ppa_core.c:128 | 见源码上下文 | True | `heap_caps_aligned_calloc(alignment, 1, s_platform.dma_desc_mem_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ppa/src/ppa_core.c:272 | ppa_register_client | True | `heap_caps_calloc(1, sizeof(ppa_client_t), PPA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ppa/src/ppa_core.c:277 | ppa_register_client | True | `xQueueCreateWithCaps(queue_size, sizeof(uint32_t), PPA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_ppa/src/ppa_core.c:359 | ppa_malloc_transaction | True | `heap_caps_calloc(1, trans_elm_storage_size, PPA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_rmt/src/rmt_common.c:44 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(rmt_group_t), RMT_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_rmt/src/rmt_encoder.c:24 | rmt_alloc_encoder_mem | True | `heap_caps_calloc(1, size, RMT_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_rmt/src/rmt_rx.c:199 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(rmt_rx_channel_t), mem_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_rmt/src/rmt_rx.c:212 | 见源码上下文 | True | `heap_caps_aligned_calloc(RMT_DMA_DESC_ALIGN, num_dma_nodes, sizeof(rmt_dma_descriptor_t), mem_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_rmt/src/rmt_tx.c:64 | rmt_tx_init_dma_link | True | `heap_caps_aligned_calloc(int_alignment, sizeof(rmt_symbol_word_t), config->mem_block_symbols, RMT_MEM_ALLOC_CAPS &#124; MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_rmt/src/rmt_tx.c:173 | rmt_tx_create_trans_queue | True | `xQueueCreateWithCaps(config->trans_queue_depth, sizeof(rmt_tx_trans_desc_t *), RMT_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_rmt/src/rmt_tx.c:262 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(rmt_tx_channel_t) + sizeof(rmt_tx_trans_desc_t) * config->trans_queue_depth, mem_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_rmt/src/rmt_tx.c:270 | 见源码上下文 | True | `heap_caps_aligned_calloc(RMT_DMA_DESC_ALIGN, RMT_DMA_NODES_PING_PONG, sizeof(rmt_dma_descriptor_t), mem_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_rmt/src/rmt_tx.c:402 | rmt_new_sync_manager | True | `heap_caps_calloc(1, sizeof(rmt_sync_manager_t) + sizeof(rmt_channel_handle_t) * config->array_size, RMT_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_sdm/src/sdm.c:91 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(sdm_group_t), SDM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_sdm/src/sdm.c:204 | sdm_new_channel | True | `heap_caps_calloc(1, sizeof(sdm_channel_t), SDM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_sdmmc/src/sdmmc_host.c:528 | 见源码上下文 | True | `xQueueCreate(SDMMC_EVENT_QUEUE_LENGTH, sizeof(sdmmc_event_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_common.c:285 | spicommon_dma_chan_alloc | True | `heap_caps_calloc(1, sizeof(spi_dma_ctx_t), SPI_COMMON_MALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_common.c:310 | 见源码上下文 | True | `heap_caps_aligned_calloc(DMA_DESC_MEM_ALIGN_SIZE, 1, sizeof(spi_dma_desc_t) * dma_desc_ct, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_common.c:311 | 见源码上下文 | True | `heap_caps_aligned_calloc(DMA_DESC_MEM_ALIGN_SIZE, 1, sizeof(spi_dma_desc_t) * dma_desc_ct, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_common.c:845 | spi_bus_initialize | True | `heap_caps_calloc(1, sizeof(spicommon_bus_context_t), SPI_COMMON_MALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_common.c:960 | 见源码上下文 | True | `heap_caps_aligned_calloc(alignment, 1, size, extra_heap_caps &#124; MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:302 | spi_master_init_driver | True | `heap_caps_malloc(sizeof(spi_host_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:527 | 见源码上下文 | True | `heap_caps_malloc(sizeof(spi_device_t), SPI_MASTER_MALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:537 | 见源码上下文 | True | `xQueueCreate(dev_config->queue_size, sizeof(spi_trans_priv_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:543 | 见源码上下文 | True | `xQueueCreate(dev_config->queue_size, sizeof(spi_trans_priv_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:1213 | 见源码上下文 | True | `heap_caps_aligned_alloc(alignment, align_len, mem_cap)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_master.c:1850 | spi_device_queue_multi_trans | True | `heap_caps_malloc(trans_num * SOC_SPI_SCT_BUFFER_NUM_MAX * sizeof(uint32_t), MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_slave.c:180 | 见源码上下文 | True | `heap_caps_malloc(sizeof(spi_slave_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_slave.c:278 | 见源码上下文 | True | `xQueueCreate(slave_config->queue_size, sizeof(spi_slave_trans_priv_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_slave.c:284 | 见源码上下文 | True | `xQueueCreate(slave_config->queue_size, sizeof(spi_slave_trans_priv_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_slave.c:442 | 见源码上下文 | True | `heap_caps_aligned_alloc(alignment, buffer_byte_len, MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_slave.c:459 | 见源码上下文 | True | `heap_caps_aligned_alloc(alignment, buffer_byte_len, MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_slave_hd.c:121 | spi_slave_hd_init | True | `heap_caps_calloc(1, sizeof(spi_slave_hd_slot_t), MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_slave_hd.c:152 | 见源码上下文 | True | `heap_caps_malloc(sizeof(spi_slave_hd_hal_desc_append_t) * host->hal.dma_desc_num, MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_slave_hd.c:153 | 见源码上下文 | True | `heap_caps_malloc(sizeof(spi_slave_hd_hal_desc_append_t) * host->hal.dma_desc_num, MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_slave_hd.c:232 | 见源码上下文 | True | `xQueueCreate(config->queue_size, sizeof(spi_slave_hd_trans_priv_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_slave_hd.c:233 | 见源码上下文 | True | `xQueueCreate(config->queue_size, sizeof(spi_slave_hd_trans_priv_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_slave_hd.c:235 | 见源码上下文 | True | `xQueueCreate(config->queue_size, sizeof(spi_slave_hd_trans_priv_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_slave_hd.c:236 | 见源码上下文 | True | `xQueueCreate(config->queue_size, sizeof(spi_slave_hd_trans_priv_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_spi/src/gpspi/spi_slave_hd.c:711 | 见源码上下文 | True | `heap_caps_aligned_alloc(64, byte_len, MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_tsens/src/temperature_sensor.c:54 | temperature_sensor_attribute_table_sort | True | `heap_caps_malloc(sizeof(temperature_sensor_attributes), TEMPERATURE_SENSOR_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_tsens/src/temperature_sensor.c:138 | temperature_sensor_install | True | `heap_caps_calloc(1, sizeof(temperature_sensor_obj_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_tsens/src/temperature_sensor_etm.c:47 | temperature_sensor_new_etm_event | True | `heap_caps_calloc(1, sizeof(esp_etm_event_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_tsens/src/temperature_sensor_etm.c:77 | temperature_sensor_new_etm_task | True | `heap_caps_calloc(1, sizeof(esp_etm_task_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_twai/esp_twai_onchip.c:685 | twai_new_node_onchip | True | `heap_caps_calloc(1, sizeof(twai_onchip_ctx_t) + twai_hal_get_mem_requirment(), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_twai/esp_twai_onchip.c:698 | twai_new_node_onchip | True | `xQueueCreateWithCaps(node_config->tx_queue_depth, sizeof(twai_frame_t *), TWAI_MALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uart.c:623 | uart_pattern_queue_reset | True | `heap_caps_malloc(queue_length * sizeof(int), UART_MALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uart.c:1891 | uart_alloc_driver_obj | True | `heap_caps_calloc(1, sizeof(uart_obj_t), UART_MALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uart.c:1895 | 见源码上下文 | True | `heap_caps_calloc(UART_HW_FIFO_LEN(uart_num), sizeof(uint32_t), UART_MALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uart.c:1900 | 见源码上下文 | True | `xQueueCreateWithCaps(event_queue_size, sizeof(uart_event_t), UART_MALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uart.c:1906 | 见源码上下文 | True | `xRingbufferCreateWithCaps(tx_buffer_size, RINGBUF_TYPE_NOSPLIT, UART_MALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uart.c:1911 | 见源码上下文 | True | `xRingbufferCreateWithCaps(rx_buffer_size, RINGBUF_TYPE_BYTEBUF, UART_MALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uart_vfs.c:423 | 见源码上下文 | True | `heap_caps_realloc(s_registered_selects, new_size * sizeof(uart_select_args_t *), UART_VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uart_vfs.c:449 | 见源码上下文 | True | `heap_caps_realloc(s_registered_selects, new_size * sizeof(uart_select_args_t *), UART_VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uart_vfs.c:506 | 见源码上下文 | True | `heap_caps_malloc(sizeof(uart_select_args_t), UART_VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uhci.c:242 | uhci_gdma_initialize | True | `heap_caps_calloc(uhci_ctrl->rx_dir.rx_num_dma_nodes, sizeof(*uhci_ctrl->rx_dir.buffer_size_per_desc_node), UHCI_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uhci.c:244 | uhci_gdma_initialize | True | `heap_caps_calloc(uhci_ctrl->rx_dir.rx_num_dma_nodes, sizeof(*uhci_ctrl->rx_dir.buffer_pointers), UHCI_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uhci.c:486 | uhci_new_controller | True | `heap_caps_calloc(1, sizeof(uhci_controller_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uhci.c:510 | 见源码上下文 | True | `xQueueCreateWithCaps(config->tx_trans_queue_depth, sizeof(uhci_transaction_desc_t *), UHCI_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_uart/src/uhci.c:514 | 见源码上下文 | True | `heap_caps_calloc(config->tx_trans_queue_depth, sizeof(uhci_transaction_desc_t), UHCI_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_usb_serial_jtag/src/usb_serial_jtag.c:169 | usb_serial_jtag_driver_install | True | `heap_caps_calloc(1, sizeof(usb_serial_jtag_obj_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_usb_serial_jtag/src/usb_serial_jtag.c:178 | 见源码上下文 | True | `xRingbufferCreate(usb_serial_jtag_config->rx_buffer_size, RINGBUF_TYPE_BYTEBUF)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_usb_serial_jtag/src/usb_serial_jtag.c:185 | 见源码上下文 | True | `xRingbufferCreate(usb_serial_jtag_config->tx_buffer_size, RINGBUF_TYPE_BYTEBUF)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c:416 | 见源码上下文 | True | `heap_caps_realloc(s_registered_selects, new_size * sizeof(usb_serial_jtag_select_args_t *), USJ_VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c:447 | 见源码上下文 | True | `heap_caps_realloc(s_registered_selects, new_size * sizeof(usb_serial_jtag_select_args_t *), USJ_VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_vfs.c:474 | 见源码上下文 | True | `heap_caps_malloc(sizeof(usb_serial_jtag_select_args_t), USJ_VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/esp_eth.c:211 | esp_eth_driver_install | True | `heap_caps_calloc(1, sizeof(esp_eth_driver_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/esp_eth_netif_glue.c:222 | esp_eth_new_netif_glue | True | `calloc(1, sizeof(esp_eth_netif_glue_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/mac/esp_eth_mac_esp.c:705 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(emac_esp32_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/mac/esp_eth_mac_esp.c:707 | 见源码上下文 | True | `calloc(1, sizeof(emac_esp32_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/mac/esp_eth_mac_esp.c:726 | 见源码上下文 | True | `xTaskCreatePinnedToCore(emac_esp32_rx_task, "emac_rx", config->rx_task_stack_size, emac, config->rx_task_prio, &emac->rx_task_hdl, core_num)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/mac/esp_eth_mac_esp_dma.c:383 | 见源码上下文 | True | `malloc(copy_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/mac/esp_eth_mac_esp_dma.c:500 | emac_esp_new_dma | True | `calloc(1, sizeof(struct emac_esp_dma_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/mac/esp_eth_mac_esp_dma.c:506 | emac_esp_new_dma | True | `heap_caps_aligned_calloc(4, 1, desc_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/mac/esp_eth_mac_esp_dma.c:510 | emac_esp_new_dma | True | `heap_caps_aligned_calloc(4, 1, CONFIG_ETH_DMA_BUFFER_SIZE, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/mac/esp_eth_mac_esp_dma.c:514 | emac_esp_new_dma | True | `heap_caps_aligned_calloc(4, 1, CONFIG_ETH_DMA_BUFFER_SIZE, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/phy/esp_eth_phy_dp83848.c:178 | esp_eth_phy_new_dp83848 | True | `calloc(1, sizeof(phy_dp83848_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/phy/esp_eth_phy_generic.c:22 | esp_eth_phy_new_generic | True | `calloc(1, sizeof(phy_802_3_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/phy/esp_eth_phy_ip101.c:199 | esp_eth_phy_new_ip101 | True | `calloc(1, sizeof(phy_ip101_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/phy/esp_eth_phy_ksz80xx.c:203 | esp_eth_phy_new_ksz80xx | True | `calloc(1, sizeof(phy_ksz80xx_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/phy/esp_eth_phy_lan87xx.c:364 | esp_eth_phy_new_lan87xx | True | `calloc(1, sizeof(phy_lan87xx_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_eth/src/phy/esp_eth_phy_rtl8201.c:175 | esp_eth_phy_new_rtl8201 | True | `calloc(1, sizeof(phy_rtl8201_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_event/esp_event.c:151 | handler_instances_add | True | `calloc(1, sizeof(*handler_instance))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_event/esp_event.c:157 | 见源码上下文 | True | `calloc(1, sizeof(*context))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_event/esp_event.c:219 | 见源码上下文 | True | `calloc(1, sizeof(*id_node))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_event/esp_event.c:274 | 见源码上下文 | True | `calloc(1, sizeof(*base_node))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_event/esp_event.c:527 | 见源码上下文 | True | `calloc(1, sizeof(esp_event_handler_instance_context_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_event/esp_event.c:555 | 见源码上下文 | True | `calloc(1, sizeof(*loop))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_event/esp_event.c:561 | 见源码上下文 | True | `xQueueCreate(event_loop_args->queue_size, sizeof(esp_event_post_instance_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_event/esp_event.c:577 | 见源码上下文 | True | `xTaskCreatePinnedToCore(esp_event_loop_run_task, event_loop_args->task_name, event_loop_args->task_stack_size, (void*) loop, event_loop_args->task_priority, &(loop->task), event_loop_args->task_core_id)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_event/esp_event.c:815 | 见源码上下文 | True | `calloc(1, sizeof(*loop_node))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_event/esp_event.c:928 | 见源码上下文 | True | `calloc(1, event_data_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_event/esp_event.c:1048 | esp_event_dump | True | `calloc(sz, sizeof(char))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/esp_http_client.c:319 | 见源码上下文 | True | `realloc(res_buffer->orig_raw_data, res_buffer->raw_len + length)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/esp_http_client.c:748 | 见源码上下文 | True | `calloc(1, sizeof(struct ifreq))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/esp_http_client.c:788 | esp_http_client_init | True | `calloc(1, sizeof(esp_http_client_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/esp_http_client.c:789 | esp_http_client_init | True | `calloc(1, sizeof(struct http_parser))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/esp_http_client.c:790 | esp_http_client_init | True | `calloc(1, sizeof(struct http_parser_settings))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/esp_http_client.c:791 | esp_http_client_init | True | `calloc(1, sizeof(esp_http_auth_data_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/esp_http_client.c:792 | esp_http_client_init | True | `calloc(1, sizeof(esp_http_data_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/esp_http_client.c:794 | esp_http_client_init | True | `calloc(1, sizeof(esp_http_buffer_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/esp_http_client.c:795 | esp_http_client_init | True | `calloc(1, sizeof(esp_http_data_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/esp_http_client.c:797 | esp_http_client_init | True | `calloc(1, sizeof(esp_http_buffer_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/esp_http_client.c:929 | 见源码上下文 | True | `malloc(client->buffer_size_tx)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/esp_http_client.c:930 | 见源码上下文 | True | `malloc(client->buffer_size_rx)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/lib/http_auth.c:138 | 见源码上下文 | True | `calloc(1, digest_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/lib/http_auth.c:141 | 见源码上下文 | True | `calloc(1, digest_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/lib/http_auth.c:144 | 见源码上下文 | True | `calloc(1, digest_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/lib/http_auth.c:237 | 见源码上下文 | True | `calloc(1, 6 + n + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/lib/http_header.c:36 | http_header_init | True | `calloc(1, sizeof(struct http_header))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/lib/http_header.c:82 | http_header_new_item | True | `calloc(1, sizeof(http_header_item_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/lib/http_utils.c:29 | 见源码上下文 | True | `calloc(1, first_str_len + second_str_len + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/lib/http_utils.c:48 | 见源码上下文 | True | `realloc(old_str, l + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/lib/http_utils.c:52 | 见源码上下文 | True | `calloc(1, l + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/lib/http_utils.c:71 | 见源码上下文 | True | `realloc(old_str, old_len + l + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/lib/http_utils.c:76 | 见源码上下文 | True | `calloc(1, l + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/lib/http_utils.c:122 | 见源码上下文 | True | `calloc(1, found_end - found + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_client/lib/http_utils.c:138 | 见源码上下文 | True | `calloc(1, found_end - found + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_main.c:427 | httpd_create | True | `calloc(1, sizeof(struct httpd_data))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_main.c:432 | 见源码上下文 | True | `calloc(config->max_uri_handlers, sizeof(httpd_uri_t *))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_main.c:438 | 见源码上下文 | True | `calloc(config->max_open_sockets, sizeof(struct sock_db))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_main.c:446 | 见源码上下文 | True | `calloc(config->max_resp_headers, sizeof(struct resp_hdr))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_main.c:454 | 见源码上下文 | True | `calloc(HTTPD_ERR_CODE_MAX, sizeof(httpd_err_handler_func_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_parse.c:506 | 见源码上下文 | True | `realloc(raux->scratch, offset + buf_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_parse.c:1202 | 见源码上下文 | True | `malloc(hdr_len_cookie + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_txrx.c:281 | 见源码上下文 | True | `malloc(required_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_txrx.c:367 | 见源码上下文 | True | `malloc(required_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_txrx.c:657 | 见源码上下文 | True | `malloc(sizeof(httpd_req_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_txrx.c:664 | 见源码上下文 | True | `malloc(sizeof(struct httpd_req_aux))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_txrx.c:677 | 见源码上下文 | True | `malloc(r_aux->scratch_cur_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_txrx.c:688 | 见源码上下文 | True | `calloc(hd->config.max_resp_headers, sizeof(struct resp_hdr))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_uri.c:150 | 见源码上下文 | True | `malloc(sizeof(httpd_uri_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_ws.c:584 | httpd_ws_send_data | True | `calloc(1, sizeof(async_transfer_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_http_server/src/httpd_ws.c:619 | httpd_ws_send_data_async | True | `calloc(1, sizeof(async_transfer_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/debug_probe/debug_probe.c:76 | 见源码上下文 | True | `calloc(1, sizeof(debug_probe_unit_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/debug_probe/debug_probe.c:164 | 见源码上下文 | True | `calloc(1, sizeof(debug_probe_channel_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/async_memcpy_gdma.c:113 | esp_async_memcpy_install_gdma_template | True | `heap_caps_calloc(1, sizeof(async_memcpy_gdma_context_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/async_memcpy_gdma.c:117 | esp_async_memcpy_install_gdma_template | True | `heap_caps_calloc(trans_queue_len, sizeof(async_memcpy_transaction_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/dma2d.c:367 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(dma2d_group_t), DMA2D_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/dma2d.c:368 | 见源码上下文 | True | `heap_caps_calloc(DMA2D_LL_TX_CHANNELS_PER_GROUP, sizeof(dma2d_tx_channel_t), DMA2D_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/dma2d.c:369 | 见源码上下文 | True | `heap_caps_calloc(DMA2D_LL_RX_CHANNELS_PER_GROUP, sizeof(dma2d_rx_channel_t), DMA2D_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/dw_gdma.c:113 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(dw_gdma_group_t), DW_GDMA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/dw_gdma.c:242 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(dw_gdma_channel_t), DW_GDMA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/dw_gdma.c:388 | dw_gdma_new_link_list | True | `heap_caps_calloc(1, sizeof(dw_gdma_link_list_t), DW_GDMA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/dw_gdma.c:392 | dw_gdma_new_link_list | True | `heap_caps_aligned_calloc(DW_GDMA_LL_LINK_LIST_ALIGNMENT, num_items, sizeof(dw_gdma_link_list_item_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/esp_dma_utils.c:53 | 见源码上下文 | True | `heap_caps_calloc(2, split_line_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/esp_dma_utils.c:195 | 见源码上下文 | True | `heap_caps_aligned_alloc(alignment_bytes, size, heap_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/gdma.c:91 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(gdma_tx_channel_t), GDMA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/gdma.c:95 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(gdma_rx_channel_t), GDMA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/gdma.c:673 | gdma_acquire_group_handle | True | `heap_caps_calloc(1, sizeof(gdma_group_t), GDMA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/gdma.c:739 | gdma_acquire_pair_handle | True | `heap_caps_calloc(1, sizeof(gdma_pair_t), GDMA_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/gdma_etm.c:43 | gdma_new_etm_event | True | `heap_caps_calloc(1, sizeof(esp_etm_event_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/gdma_etm.c:77 | gdma_new_etm_task | True | `heap_caps_calloc(1, sizeof(gdma_etm_task_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/gdma_link.c:73 | gdma_new_link_list | True | `heap_caps_calloc(1, sizeof(gdma_link_list_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/dma/gdma_link.c:96 | 见源码上下文 | True | `heap_caps_aligned_calloc(item_alignment, num_items, item_size, list_items_mem_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/esp_clock_output.c:127 | 见源码上下文 | True | `malloc(sizeof(esp_clock_output_mapping_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/esp_etm.c:110 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(etm_group_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/esp_etm.c:252 | esp_etm_new_channel | True | `heap_caps_calloc(1, sizeof(esp_etm_channel_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:172 | 见源码上下文 | True | `heap_caps_malloc(sizeof(vector_desc_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:574 | 见源码上下文 | True | `heap_caps_malloc(sizeof(intr_handle_data_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:611 | 见源码上下文 | True | `heap_caps_malloc(sizeof(shared_vector_desc_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/intr_alloc.c:634 | 见源码上下文 | True | `heap_caps_malloc(sizeof(non_shared_isr_arg_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/lowpower/port/esp32p4/sleep_cpu.c:95 | cpu_domain_dev_sleep_frame_alloc_and_init | True | `heap_caps_malloc(sizeof(cpu_domain_dev_sleep_frame_t) + region_sz + regs_frame_sz, MALLOC_CAP_32BIT&#124;MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/lowpower/port/esp32p4/sleep_cpu.c:125 | 见源码上下文 | True | `heap_caps_calloc(1, RV_SLEEP_CTX_FRMSZ, MALLOC_CAP_32BIT&#124;MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/lowpower/port/esp32p4/sleep_cpu.c:133 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(RvCoreNonCriticalSleepFrame), MALLOC_CAP_32BIT&#124;MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/port/regdma_link.c:32 | REGDMA_LINK_ADDR_ALIGN | True | `heap_caps_aligned_alloc( REGDMA_LINK_ADDR_ALIGN, buff ? sizeof(regdma_link_continuous_t) : (sizeof(regdma_link_continuous_t) + (len<<2)), REGDMA_LINK_MEM_TYPE_CAPS )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/port/regdma_link.c:48 | regdma_link_new_addr_map | True | `heap_caps_aligned_alloc( REGDMA_LINK_ADDR_ALIGN, buff ? sizeof(regdma_link_addr_map_t) : (sizeof(regdma_link_addr_map_t) + (len<<2)), REGDMA_LINK_MEM_TYPE_CAPS )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/port/regdma_link.c:64 | regdma_link_new_write | True | `heap_caps_aligned_alloc( REGDMA_LINK_ADDR_ALIGN, sizeof(regdma_link_write_wait_t), REGDMA_LINK_MEM_TYPE_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/port/regdma_link.c:76 | regdma_link_new_wait | True | `heap_caps_aligned_alloc( REGDMA_LINK_ADDR_ALIGN, sizeof(regdma_link_write_wait_t), REGDMA_LINK_MEM_TYPE_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/port/regdma_link.c:88 | regdma_link_new_branch_continuous | True | `heap_caps_aligned_alloc( REGDMA_LINK_ADDR_ALIGN, buff ? sizeof(regdma_link_branch_continuous_t) : (sizeof(regdma_link_branch_continuous_t) + (len<<2)), REGDMA_LINK_MEM_TYPE_CAPS )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/port/regdma_link.c:104 | regdma_link_new_branch_addr_map | True | `heap_caps_aligned_alloc( REGDMA_LINK_ADDR_ALIGN, buff ? sizeof(regdma_link_branch_addr_map_t) : (sizeof(regdma_link_branch_addr_map_t) + (len<<2)), REGDMA_LINK_MEM_TYPE_CAPS )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/port/regdma_link.c:120 | regdma_link_new_branch_write | True | `heap_caps_aligned_alloc( REGDMA_LINK_ADDR_ALIGN, sizeof(regdma_link_branch_write_wait_t), REGDMA_LINK_MEM_TYPE_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/port/regdma_link.c:132 | regdma_link_new_branch_wait | True | `heap_caps_aligned_alloc( REGDMA_LINK_ADDR_ALIGN, sizeof(regdma_link_branch_write_wait_t), REGDMA_LINK_MEM_TYPE_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/rtc_module.c:121 | 见源码上下文 | True | `heap_caps_malloc(sizeof(*item), MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/sleep_event.c:29 | 见源码上下文 | True | `heap_caps_malloc(sizeof(esp_sleep_event_cb_config_t), MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/spi_bus_lock.c:622 | spi_bus_init_lock | True | `heap_caps_calloc(1, sizeof(spi_bus_lock_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_hw_support/spi_bus_lock.c:681 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(spi_bus_lock_dev_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/dsi/esp_lcd_mipi_dsi_bus.c:30 | esp_lcd_new_dsi_bus | True | `heap_caps_calloc(1, sizeof(esp_lcd_dsi_bus_t), DSI_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/dsi/esp_lcd_panel_dpi.c:227 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(esp_lcd_dpi_panel_t), DSI_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/dsi/esp_lcd_panel_dpi.c:238 | 见源码上下文 | True | `heap_caps_calloc(1, fb_size, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT &#124; MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/dsi/esp_lcd_panel_io_dbi.c:30 | esp_lcd_new_panel_io_dbi | True | `heap_caps_calloc(1, sizeof(esp_lcd_dbi_io_t), DSI_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/i2c/esp_lcd_panel_io_i2c_v1.c:61 | esp_lcd_new_panel_io_i2c_v1 | True | `calloc(1, sizeof(lcd_panel_io_i2c_t) + CMD_HANDLER_BUFFER_SIZE)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/i2c/esp_lcd_panel_io_i2c_v2.c:65 | esp_lcd_new_panel_io_i2c_v2 | True | `calloc(1, sizeof(lcd_panel_io_i2c_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/i80/esp_lcd_panel_io_i80.c:170 | esp_lcd_new_i80_bus | True | `heap_caps_calloc(1, sizeof(esp_lcd_i80_bus_t), LCD_I80_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/i80/esp_lcd_panel_io_i80.c:175 | esp_lcd_new_i80_bus | True | `heap_caps_calloc(1, LCD_I80_IO_FORMAT_BUF_SIZE, MALLOC_CAP_8BIT &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/i80/esp_lcd_panel_io_i80.c:354 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(lcd_panel_io_i80_t) + io_config->trans_queue_depth * sizeof(lcd_i80_trans_descriptor_t), LCD_I80_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/i80/esp_lcd_panel_io_i80.c:357 | 见源码上下文 | True | `xQueueCreate(io_config->trans_queue_depth, sizeof(lcd_i80_trans_descriptor_t *))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/i80/esp_lcd_panel_io_i80.c:359 | 见源码上下文 | True | `xQueueCreate(io_config->trans_queue_depth, sizeof(lcd_i80_trans_descriptor_t *))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/i80/esp_lcd_panel_io_i80.c:720 | 见源码上下文 | True | `heap_caps_aligned_calloc(bus->ext_mem_align, 1, size, MALLOC_CAP_8BIT &#124; MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/i80/esp_lcd_panel_io_i80.c:722 | 见源码上下文 | True | `heap_caps_aligned_calloc(bus->int_mem_align, 1, size, MALLOC_CAP_8BIT &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/parl/esp_lcd_panel_io_parl.c:94 | esp_lcd_new_panel_io_parl | True | `heap_caps_calloc(1, sizeof(lcd_panel_io_parlio_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/parl/esp_lcd_panel_io_parl.c:177 | 见源码上下文 | True | `heap_caps_aligned_calloc(ext_mem_align, 1, size, MALLOC_CAP_8BIT &#124; MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/parl/esp_lcd_panel_io_parl.c:179 | 见源码上下文 | True | `heap_caps_aligned_calloc(int_mem_align, 1, size, MALLOC_CAP_8BIT &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/rgb/esp_lcd_panel_rgb.c:179 | 见源码上下文 | True | `heap_caps_aligned_calloc(rgb_panel->ext_mem_align, 1, rgb_panel->fb_size, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_DMA &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/rgb/esp_lcd_panel_rgb.c:184 | 见源码上下文 | True | `heap_caps_aligned_calloc(rgb_panel->int_mem_align, 1, rgb_panel->fb_size, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_DMA &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/rgb/esp_lcd_panel_rgb.c:199 | 见源码上下文 | True | `heap_caps_aligned_calloc(rgb_panel->int_mem_align, 1, rgb_panel->bb_size, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_DMA &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/rgb/esp_lcd_panel_rgb.c:325 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(esp_rgb_panel_t), LCD_RGB_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/spi/esp_lcd_panel_io_spi.c:78 | esp_lcd_new_panel_io_spi | True | `calloc(1, sizeof(esp_lcd_panel_io_spi_t) + sizeof(lcd_spi_trans_descriptor_t) * io_config->trans_queue_depth)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/src/esp_async_fbcpy.c:61 | esp_async_fbcpy_install | True | `heap_caps_calloc(1, sizeof(esp_async_fbcpy_context_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/src/esp_async_fbcpy.c:64 | esp_async_fbcpy_install | True | `heap_caps_calloc(1, dma2d_get_trans_elm_size(), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/src/esp_async_fbcpy.c:71 | esp_async_fbcpy_install | True | `heap_caps_aligned_calloc(alignment, 1, dma_desc_mem_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/src/esp_async_fbcpy.c:72 | esp_async_fbcpy_install | True | `heap_caps_aligned_calloc(alignment, 1, dma_desc_mem_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/src/esp_lcd_panel_nt35510.c:67 | esp_lcd_new_panel_nt35510 | True | `calloc(1, sizeof(nt35510_panel_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/src/esp_lcd_panel_ssd1306.c:79 | esp_lcd_new_panel_ssd1306 | True | `calloc(1, sizeof(ssd1306_panel_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_lcd/src/esp_lcd_panel_st7789.c:73 | esp_lcd_new_panel_st7789 | True | `calloc(1, sizeof(st7789_panel_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_mm/esp_cache_msync.c:184 | 见源码上下文 | True | `heap_caps_aligned_alloc(data_cache_line_size, size, (uint32_t)heap_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_mm/esp_mmu_map.c:486 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(mem_block_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_mm/esp_mmu_map.c:496 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(mem_block_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_mm/esp_mmu_map.c:550 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(mem_block_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_netif/esp_netif_objects.c:37 | esp_netif_add_to_list_unsafe | True | `calloc(1, sizeof(struct slist_netifs_s))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_netif/lwip/esp_netif_lwip.c:791 | 见源码上下文 | True | `calloc(1, sizeof(struct esp_netif_obj))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_netif/lwip/esp_netif_lwip.c:799 | 见源码上下文 | True | `calloc(1, sizeof(esp_netif_ip_info_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_netif/lwip/esp_netif_lwip.c:809 | 见源码上下文 | True | `calloc(1, sizeof(esp_netif_ip_info_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_netif/lwip/esp_netif_lwip.c:820 | 见源码上下文 | True | `calloc(1, sizeof(struct netif))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_netif/lwip/esp_netif_lwip.c:943 | 见源码上下文 | True | `calloc(1, sizeof(struct netif))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_netif/lwip/esp_netif_sntp.c:95 | esp_netif_sntp_init | True | `calloc(1, sizeof(sntp_storage_t) + (config->renew_servers_after_new_IP ? config->num_of_servers * sizeof(char*) : 0))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_partition/partition.c:169 | 见源码上下文 | True | `calloc(1, sizeof(partition_list_item_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_partition/partition.c:291 | iterator_create | True | `malloc(sizeof(esp_partition_iterator_opaque_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_partition/partition.c:428 | 见源码上下文 | True | `calloc(1, sizeof(partition_list_item_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_pm/pm_impl.c:289 | 见源码上下文 | True | `heap_caps_malloc(sizeof(esp_pm_sleep_cb_config_t), MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_pm/pm_impl.c:313 | 见源码上下文 | True | `heap_caps_malloc(sizeof(esp_pm_sleep_cb_config_t), MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_pm/pm_locks.c:60 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(*new_lock), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_psram/system_layer/esp_psram.c:141 | 见源码上下文 | True | `heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_psram/system_layer/esp_psram.c:648 | 见源码上下文 | True | `heap_caps_malloc(next_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_psram/system_layer/esp_psram_mspi.c:29 | esp_psram_mspi_mb_init | True | `heap_caps_calloc(1, CONFIG_CACHE_L1_CACHE_LINE_SIZE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_CACHE_ALIGNED)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_ringbuf/ringbuf.c:942 | 见源码上下文 | True | `xRingbufferCreate(size_t xBufferSize, RingbufferType_t xBufferType)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_ringbuf/ringbuf.c:951 | 见源码上下文 | True | `calloc(1, sizeof(Ringbuffer_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_ringbuf/ringbuf.c:952 | 见源码上下文 | True | `malloc(xBufferSize)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_ringbuf/ringbuf.c:967 | 见源码上下文 | True | `xRingbufferCreateNoSplit(size_t xItemSize, size_t xItemNum)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_ringbuf/ringbuf.c:969 | xRingbufferCreateNoSplit | True | `xRingbufferCreate((rbALIGN_SIZE(xItemSize) + rbHEADER_SIZE) * xItemNum, RINGBUF_TYPE_NOSPLIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_ringbuf/ringbuf.c:972 | xRingbufferCreateNoSplit | True | `xRingbufferCreateStatic(size_t xBufferSize, RingbufferType_t xBufferType, uint8_t *pucRingbufferStorage, StaticRingbuffer_t *pxStaticRingbuffer)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_ringbuf/ringbuf.c:1461 | 见源码上下文 | True | `xRingbufferCreateWithCaps(size_t xBufferSize, RingbufferType_t xBufferType, UBaseType_t uxMemoryCaps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_ringbuf/ringbuf.c:1472 | 见源码上下文 | True | `heap_caps_malloc(sizeof(StaticRingbuffer_t), (uint32_t)uxMemoryCaps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_ringbuf/ringbuf.c:1473 | 见源码上下文 | True | `heap_caps_malloc(xBufferSize, (uint32_t)uxMemoryCaps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_ringbuf/ringbuf.c:1480 | 见源码上下文 | True | `xRingbufferCreateStatic(xBufferSize, xBufferType, pucRingbufferStorage, pxStaticRingbuffer)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_security/src/esp_ds.c:141 | 见源码上下文 | True | `malloc(sizeof(esp_ds_context_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_security/src/esp_ds.c:379 | 见源码上下文 | True | `malloc(sizeof(esp_ds_context_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_security/src/esp_key_mgr.c:241 | deploy_huk | True | `heap_caps_calloc(1, KEY_MGR_HUK_INFO_SIZE, MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_security/src/esp_key_mgr.c:323 | 见源码上下文 | True | `heap_caps_calloc(1, KEY_MGR_KEY_RECOVERY_INFO_SIZE, MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_security/src/esp_key_mgr.c:642 | 见源码上下文 | True | `heap_caps_calloc(1, KEY_MGR_KEY_RECOVERY_INFO_SIZE, MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_security/src/esp_key_mgr.c:796 | 见源码上下文 | True | `heap_caps_calloc(1, KEY_MGR_KEY_RECOVERY_INFO_SIZE, MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_system/esp_ipc.c:114 | esp_ipc_init | True | `xTaskCreatePinnedToCore(ipc_task, task_name, IPC_STACK_SIZE, (void*) i, IPC_MAX_PRIORITY, &s_ipc_task_handle[i], i)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_system/systick_etm.c:31 | esp_systick_new_etm_alarm_event | True | `heap_caps_calloc(1, sizeof(esp_etm_event_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_system/task_wdt/task_wdt.c:179 | add_entry | True | `calloc(1, sizeof(twdt_entry_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_system/task_wdt/task_wdt.c:522 | esp_task_wdt_init | True | `calloc(1, sizeof(twdt_obj_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_timer/src/esp_timer.c:110 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(*result), MALLOC_CAP_8BIT &#124; MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_timer/src/esp_timer.c:506 | 见源码上下文 | True | `xTaskCreatePinnedToCore( &timer_task, "esp_timer", ESP_TASK_TIMER_STACK, NULL, ESP_TASK_TIMER_PRIO, &s_timer_task, CONFIG_ESP_TIMER_TASK_AFFINITY)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_timer/src/esp_timer.c:654 | LIST_FOREACH | True | `calloc(1, buf_size + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_timer/src/esp_timer_etm.c:30 | esp_timer_new_etm_alarm_event | True | `heap_caps_calloc(1, sizeof(esp_etm_event_t), ETM_MEM_ALLOC_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/esp_wifi/src/wifi_netif.c:105 | esp_wifi_create_if_driver | True | `calloc(1, sizeof(struct wifi_netif_driver))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/fatfs/diskio/diskio.c:80 | 见源码上下文 | True | `malloc(sizeof(ff_diskio_impl_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/fatfs/port/freertos/ffsystem.c:23 | ff_memalloc | True | `heap_caps_malloc_prefer(msize, 2, MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/fatfs/port/freertos/ffsystem.c:26 | ff_memalloc | True | `malloc(msize)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/fatfs/vfs/vfs_fat_sdmmc.c:96 | 见源码上下文 | True | `malloc(sizeof(sdmmc_card_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/fatfs/vfs/vfs_fat_sdmmc.c:295 | 见源码上下文 | True | `calloc(1, sizeof(vfs_fat_sd_ctx_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/fatfs/vfs/vfs_fat_sdmmc.c:393 | 见源码上下文 | True | `calloc(1, sizeof(vfs_fat_sd_ctx_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/fatfs/vfs/vfs_fat_spiflash.c:197 | 见源码上下文 | True | `calloc(1, sizeof(vfs_fat_spiflash_ctx_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/app_startup.c:83 | esp_startup_start_app | True | `xTaskCreatePinnedToCore(main_task, "main", ESP_TASK_MAIN_STACK, NULL, ESP_TASK_MAIN_PRIO, NULL, ESP_TASK_MAIN_CORE)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:31 | ? | True | `xTaskCreatePinnedToCoreWithCaps( TaskFunction_t pvTaskCode, const char * const pcName, const configSTACK_DEPTH_TYPE usStackDepth, void * const pvParameters, UBaseType_t uxPriority, TaskHandle_t * const pvCreatedTask, const BaseType_t xCoreID, UBaseType_t uxMemoryCaps )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:50 | 见源码上下文 | True | `heap_caps_malloc( ( ( size_t ) usStackDepth ) * sizeof( StackType_t ), ( uint32_t ) uxMemoryCaps )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:58 | 见源码上下文 | True | `xTaskCreateStaticPinnedToCore( pvTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxStack, pxTaskBuffer, xCoreID )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:190 | 见源码上下文 | True | `xTaskCreatePinnedToCore( ( TaskFunction_t ) prvTaskDeleteWithCapsTask, "prvTaskDeleteWithCapsTask", configMINIMAL_STACK_SIZE, xCurrentTaskHandle, uxTaskPriorityGet( xTaskToDelete ), NULL, xPortGetCoreID() )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:219 | 见源码上下文 | True | `xQueueCreateWithCaps( UBaseType_t uxQueueLength, UBaseType_t uxItemSize, UBaseType_t uxMemoryCaps )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:228 | 见源码上下文 | True | `heap_caps_malloc( sizeof( StaticQueue_t ), ( uint32_t ) uxMemoryCaps )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:236 | 见源码上下文 | True | `heap_caps_malloc( uxQueueLength * uxItemSize, ( uint32_t ) uxMemoryCaps )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:245 | 见源码上下文 | True | `xQueueCreateStatic( uxQueueLength, uxItemSize, pucQueueStorageBuffer, pxQueueBuffer )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:300 | 见源码上下文 | True | `heap_caps_malloc( sizeof( StaticSemaphore_t ), ( uint32_t ) uxMemoryCaps )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:374 | 见源码上下文 | True | `heap_caps_malloc( sizeof( StaticStreamBuffer_t ), ( uint32_t ) uxMemoryCaps )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions.c:375 | 见源码上下文 | True | `heap_caps_malloc( xBufferSizeBytes, ( uint32_t ) uxMemoryCaps )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/esp_additions/idf_additions_event_groups.c:37 | 见源码上下文 | True | `heap_caps_malloc( sizeof( StaticEventGroup_t ), uxMemoryCaps )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:664 | 见源码上下文 | True | `xQueueCreateMutex( const uint8_t ucQueueType )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:680 | 见源码上下文 | True | `xQueueCreateMutexStatic( const uint8_t ucQueueType, StaticQueue_t * pxStaticQueue )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:858 | 见源码上下文 | True | `xQueueCreateCountingSemaphoreStatic( const UBaseType_t uxMaxCount, const UBaseType_t uxInitialCount, StaticQueue_t * pxStaticQueue )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:894 | 见源码上下文 | True | `xQueueCreateCountingSemaphore( const UBaseType_t uxMaxCount, const UBaseType_t uxInitialCount )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/queue.c:3239 | 见源码上下文 | True | `xQueueCreateSet( const UBaseType_t uxEventQueueLength )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/tasks.c:913 | 见源码上下文 | True | `xTaskCreateRestrictedStatic( const TaskParameters_t * const pxTaskDefinition, TaskHandle_t * pxCreatedTask )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/tasks.c:962 | 见源码上下文 | True | `xTaskCreateRestricted( const TaskParameters_t * const pxTaskDefinition, TaskHandle_t * pxCreatedTask )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/tasks.c:2337 | 见源码上下文 | True | `xTaskCreateStaticPinnedToCore( prvIdleTask, #if ( configNUMBER_OF_CORES > 1 ) cIdleName, #else configIDLE_TASK_NAME, #endif ulIdleTaskStackSize, ( void * ) NULL, portPRIVILEGE_BIT, pxIdleTaskStackBuffer, pxIdleTaskTCBBuffer, xCoreID )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/tasks.c:2362 | 见源码上下文 | True | `xTaskCreatePinnedToCore( prvIdleTask, #if ( configNUMBER_OF_CORES > 1 ) cIdleName, #else configIDLE_TASK_NAME, #endif configMINIMAL_STACK_SIZE, ( void * ) NULL, portPRIVILEGE_BIT, &xIdleTaskHandle[ xCoreID ], xCoreID )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/timers.c:280 | 见源码上下文 | True | `xTaskCreateStaticPinnedToCore( prvTimerTask, configTIMER_SERVICE_TASK_NAME, ulTimerTaskStackSize, NULL, ( ( UBaseType_t ) configTIMER_TASK_PRIORITY ) &#124; portPRIVILEGE_BIT, pxTimerTaskStackBuffer, pxTimerTaskTCBBuffer, configTIMER_SERVICE_TASK_CORE_AFFINITY )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/timers.c:296 | 见源码上下文 | True | `xTaskCreatePinnedToCore( prvTimerTask, configTIMER_SERVICE_TASK_NAME, configTIMER_TASK_STACK_DEPTH, NULL, ( ( UBaseType_t ) configTIMER_TASK_PRIORITY ) &#124; portPRIVILEGE_BIT, &xTimerTaskHandle, configTIMER_SERVICE_TASK_CORE_AFFINITY )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/timers.c:315 | 见源码上下文 | True | `xTaskCreateStatic( prvTimerTask, configTIMER_SERVICE_TASK_NAME, ulTimerTaskStackSize, NULL, ( ( UBaseType_t ) configTIMER_TASK_PRIORITY ) &#124; portPRIVILEGE_BIT, pxTimerTaskStackBuffer, pxTimerTaskTCBBuffer )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/timers.c:330 | 见源码上下文 | True | `xTaskCreate( prvTimerTask, configTIMER_SERVICE_TASK_NAME, configTIMER_TASK_STACK_DEPTH, NULL, ( ( UBaseType_t ) configTIMER_TASK_PRIORITY ) &#124; portPRIVILEGE_BIT, &xTimerTaskHandle )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/timers.c:1043 | 见源码上下文 | True | `xQueueCreateStatic( ( UBaseType_t ) configTIMER_QUEUE_LENGTH, ( UBaseType_t ) sizeof( DaemonTaskMessage_t ), &( ucStaticTimerQueueStorage[ 0 ] ), &xStaticTimerQueue )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/FreeRTOS-Kernel/timers.c:1047 | 见源码上下文 | True | `xQueueCreate( ( UBaseType_t ) configTIMER_QUEUE_LENGTH, sizeof( DaemonTaskMessage_t ) )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/freertos/heap_idf.c:55 | 见源码上下文 | True | `heap_caps_malloc(xWantedSize, portFREERTOS_HEAP_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:49 | fmt_abort_str | True | `heap_caps_alloc_failed(size_t requested_size, uint32_t caps, const char *function_name)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:62 | 见源码上下文 | True | `heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:82 | heap_caps_match | True | `heap_caps_malloc( size_t size, uint32_t caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:84 | heap_caps_malloc | True | `heap_caps_malloc_base(size, caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:88 | 见源码上下文 | True | `heap_caps_alloc_failed(size, caps, __func__)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:99 | 见源码上下文 | True | `heap_caps_malloc_extmem_enable(size_t limit)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:107 | heap_caps_malloc_extmem_enable | True | `heap_caps_malloc_default( size_t size )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:110 | 见源码上下文 | True | `heap_caps_malloc( size, MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:118 | 见源码上下文 | True | `heap_caps_malloc_base( size, MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_INTERNAL )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:120 | 见源码上下文 | True | `heap_caps_malloc_base( size, MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_SPIRAM )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:124 | 见源码上下文 | True | `heap_caps_malloc_base( size, MALLOC_CAP_DEFAULT )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:129 | 见源码上下文 | True | `heap_caps_alloc_failed(size, MALLOC_CAP_DEFAULT, __func__)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:140 | 见源码上下文 | True | `heap_caps_realloc_default( void *ptr, size_t size )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:143 | 见源码上下文 | True | `heap_caps_realloc( ptr, size, MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_INTERNAL )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:151 | 见源码上下文 | True | `heap_caps_realloc_base( ptr, size, MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:153 | 见源码上下文 | True | `heap_caps_realloc_base( ptr, size, MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_SPIRAM)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:158 | 见源码上下文 | True | `heap_caps_realloc_base( ptr, size, MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:163 | 见源码上下文 | True | `heap_caps_alloc_failed(size, MALLOC_CAP_DEFAULT, __func__)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:172 | 见源码上下文 | True | `heap_caps_malloc_prefer( size_t size, size_t num, ... )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:180 | 见源码上下文 | True | `heap_caps_malloc_base( size, caps )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:187 | 见源码上下文 | True | `heap_caps_alloc_failed(size, caps, __func__)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:196 | 见源码上下文 | True | `heap_caps_realloc_prefer( void *ptr, size_t size, size_t num, ... )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:204 | 见源码上下文 | True | `heap_caps_realloc_base( ptr, size, caps )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:211 | 见源码上下文 | True | `heap_caps_alloc_failed(size, caps, __func__)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:220 | 见源码上下文 | True | `heap_caps_calloc_prefer( size_t n, size_t size, size_t num, ... )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:228 | 见源码上下文 | True | `heap_caps_calloc_base( n, size, caps )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:235 | 见源码上下文 | True | `heap_caps_alloc_failed(size, caps, __func__)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:241 | 见源码上下文 | True | `heap_caps_realloc( void *ptr, size_t size, uint32_t caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:243 | heap_caps_realloc | True | `heap_caps_realloc_base(ptr, size, caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:247 | 见源码上下文 | True | `heap_caps_alloc_failed(size, caps, __func__)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:253 | 见源码上下文 | True | `heap_caps_calloc( size_t n, size_t size, uint32_t caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:255 | heap_caps_calloc | True | `heap_caps_calloc_base(n, size, caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:259 | 见源码上下文 | True | `heap_caps_alloc_failed(n * size, caps, __func__)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:324 | SLIST_FOREACH | True | `heap_caps_malloc(sizeof(size_t) * min_free_bytes_monitoring.counter, MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:464 | heap_caps_dump_all | True | `heap_caps_get_allocated_size( void *ptr )` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:502 | 见源码上下文 | True | `heap_caps_alloc_failed(size, caps, funcname)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:509 | 见源码上下文 | True | `heap_caps_aligned_alloc_default(size_t alignment, size_t size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:514 | 见源码上下文 | True | `heap_caps_aligned_alloc(alignment, size, MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:522 | 见源码上下文 | True | `heap_caps_aligned_alloc_base(alignment, size, MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:524 | 见源码上下文 | True | `heap_caps_aligned_alloc_base(alignment, size, MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_SPIRAM)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:531 | 见源码上下文 | True | `heap_caps_aligned_alloc_base(alignment, size, MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:534 | 见源码上下文 | True | `heap_caps_alloc_failed(size, MALLOC_CAP_DEFAULT, __func__)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:540 | 见源码上下文 | True | `heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:548 | 见源码上下文 | True | `heap_caps_aligned_alloc_base(alignment, size, caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:551 | 见源码上下文 | True | `heap_caps_alloc_failed(size, caps, __func__)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:562 | heap_caps_aligned_free | True | `heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps.c:569 | 见源码上下文 | True | `heap_caps_aligned_alloc(alignment,size_bytes, caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_base.c:104 | 见源码上下文 | True | `heap_caps_aligned_alloc_base(size_t alignment, size_t size, uint32_t caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_base.c:161 | 见源码上下文 | True | `heap_caps_update_per_task_info_alloc(heap, MULTI_HEAP_ADD_BLOCK_OWNER_OFFSET(ret), multi_heap_get_full_block_size(heap->heap, ret), get_all_caps(heap))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_base.c:179 | 见源码上下文 | True | `heap_caps_update_per_task_info_alloc(heap, MULTI_HEAP_ADD_BLOCK_OWNER_OFFSET(ret), multi_heap_get_full_block_size(heap->heap, ret), get_all_caps(heap))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_base.c:201 | 见源码上下文 | True | `heap_caps_malloc_base( size_t size, uint32_t caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_base.c:202 | heap_caps_malloc_base | True | `heap_caps_aligned_alloc_base(UNALIGNED_MEM_ALIGNMENT_BYTES, size, caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_base.c:209 | heap_caps_malloc_base | True | `heap_caps_realloc_base( void *ptr, size_t size, uint32_t caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_base.c:220 | 见源码上下文 | True | `heap_caps_aligned_alloc_base(alignment, size, caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_base.c:242 | 见源码上下文 | True | `realloc()` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_base.c:251 | 见源码上下文 | True | `realloc()` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_base.c:280 | 见源码上下文 | True | `heap_caps_update_per_task_info_realloc(heap, MULTI_HEAP_ADD_BLOCK_OWNER_OFFSET(ptr), MULTI_HEAP_ADD_BLOCK_OWNER_OFFSET(r), old_size, old_task, multi_heap_get_full_block_size(heap->heap, r), get_all_caps(heap))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_base.c:296 | 见源码上下文 | True | `heap_caps_aligned_alloc_base(alignment, size, caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_base.c:323 | 见源码上下文 | True | `heap_caps_calloc_base( size_t n, size_t size, uint32_t caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_base.c:332 | 见源码上下文 | True | `heap_caps_malloc_base(size_bytes, caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_init.c:219 | 见源码上下文 | True | `heap_caps_update_per_task_info_alloc(used_heap, MULTI_HEAP_REMOVE_BLOCK_OWNER_OFFSET(heaps_array), multi_heap_get_full_block_size(used_heap->heap, MULTI_HEAP_REMOVE_BLOCK_OWNER_OFFSET(heaps_array)), get_all_caps(used_heap))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_caps_init.c:299 | 见源码上下文 | True | `heap_caps_malloc(sizeof(heap_t), MALLOC_CAP_INTERNAL&#124;MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_trace_standalone.c:173 | 见源码上下文 | True | `heap_caps_calloc(1, map_size, MALLOC_CAP_SPIRAM)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/heap/heap_trace_standalone.c:176 | 见源码上下文 | True | `heap_caps_calloc(1, map_size, MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/json/cJSON/cJSON.c:167 | defined | True | `malloc(size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/json/cJSON/cJSON.c:175 | internal_realloc | True | `realloc(pointer, size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/log/src/log_level/tag_log_level/linked_list/log_linked_list.c:93 | add_to_list | True | `malloc(entry_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/apps/ping/ping_sock.c:226 | esp_ping_new_session | True | `xTaskCreate(esp_ping_thread, "ping", config->task_stack_size, ep, config->task_prio, &ep->ping_task_hdl)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/lwip/src/core/mem.c:213 | 见源码上下文 | True | `malloc()` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/lwip/src/netif/ppp/auth.c:512 | 见源码上下文 | True | `malloc(sizeof(struct wordlist) + l)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/lwip/src/netif/ppp/auth.c:534 | 见源码上下文 | True | `malloc(sizeof(struct wordlist) + l)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/lwip/src/netif/ppp/auth.c:2067 | 见源码上下文 | True | `malloc((n + 1) * sizeof(struct permitted_ip))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/lwip/src/netif/ppp/auth.c:2436 | 见源码上下文 | True | `malloc(sizeof(struct wordlist) + strlen(word) + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/lwip/src/netif/ppp/demand.c:92 | demand_conf | True | `malloc(framemax)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/lwip/src/netif/ppp/demand.c:299 | 见源码上下文 | True | `malloc(sizeof(struct packet) + len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/lwip/src/netif/ppp/eap.c:1211 | 见源码上下文 | True | `malloc(pl)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/lwip/src/netif/ppp/multilink.c:173 | 见源码上下文 | True | `malloc(l)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/lwip/src/netif/ppp/multilink.c:189 | 见源码上下文 | True | `malloc(l + 7)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/lwip/src/netif/ppp/multilink.c:325 | 见源码上下文 | True | `malloc(l)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/port/freertos/sys_arch.c:214 | 见源码上下文 | True | `xQueueCreate(size, sizeof(void *))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/lwip/port/freertos/sys_arch.c:390 | sys_thread_new | True | `xTaskCreatePinnedToCore(thread, name, stacksize, arg, prio, &rtos_task, CONFIG_LWIP_TCPIP_TASK_AFFINITY)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/esp_crt_bundle/esp_crt_bundle.c:249 | 见源码上下文 | True | `malloc((esp_crt_get_name_len(cert) + 1) * sizeof(char))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/aes/dma/esp_aes_dma_core.c:266 | 见源码上下文 | True | `heap_caps_aligned_alloc(input_alignment, chunk_len, input_heap_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/aes/dma/esp_aes_dma_core.c:275 | 见源码上下文 | True | `heap_caps_aligned_alloc(output_alignment, chunk_len, output_heap_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/aes/dma/esp_aes_dma_core.c:359 | ALIGN_UP | True | `heap_caps_aligned_calloc(DMA_DESC_MEM_ALIGN_SIZE, num, size, caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/aes/dma/esp_aes_dma_core.c:1125 | 见源码上下文 | True | `heap_caps_aligned_calloc(8, crypto_dma_desc_num * 2, sizeof(crypto_dma_desc_t), MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/aes/dma/esp_aes_dma_core.c:1298 | 见源码上下文 | True | `heap_caps_calloc((crypto_dma_desc_num * 2) + 1, sizeof(crypto_dma_desc_t), MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/ecdsa/ecdsa_alt.c:653 | 见源码上下文 | True | `malloc(sizeof(mbedtls_mpi_uint))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/esp_ds/esp_rsa_dec_alt.c:205 | 见源码上下文 | True | `calloc(data_len, sizeof(uint32_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/esp_ds/esp_rsa_dec_alt.c:247 | 见源码上下文 | True | `calloc(data_len, sizeof(uint32_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/esp_ds/esp_rsa_sign_alt.c:296 | 见源码上下文 | True | `heap_caps_malloc_prefer(sig_len, 2, MALLOC_CAP_32BIT &#124; MALLOC_CAP_INTERNAL, MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/esp_hmac_pbkdf2.c:30 | 见源码上下文 | True | `calloc(1, salt_len + sizeof(counter))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/esp_mem.c:17 | esp_mbedtls_mem_calloc | True | `heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL&#124;MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/esp_mem.c:19 | esp_mbedtls_mem_calloc | True | `heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM&#124;MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/esp_mem.c:26 | 见源码上下文 | True | `heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_INTERNAL&#124;MALLOC_CAP_IRAM_8BIT, MALLOC_CAP_INTERNAL&#124;MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/esp_mem.c:28 | 见源码上下文 | True | `heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL&#124;MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/esp_mem.c:32 | 见源码上下文 | True | `calloc(n, size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/sha/core/sha.c:174 | 见源码上下文 | True | `heap_caps_aligned_alloc(SOC_GDMA_EXT_MEM_ENC_ALIGNMENT, ilen, heap_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/sha/core/sha.c:187 | 见源码上下文 | True | `heap_caps_aligned_alloc(SOC_GDMA_EXT_MEM_ENC_ALIGNMENT, buf_len, heap_caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mbedtls/port/sha/core/sha.c:343 | 见源码上下文 | True | `heap_caps_malloc(sizeof(unsigned char) * buf_len, MALLOC_CAP_8BIT&#124;MALLOC_CAP_DMA&#124;MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/lib/mqtt_msg.c:623 | mqtt_msg_buffer_init | True | `calloc(buffer_size, sizeof(uint8_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/lib/mqtt_outbox.c:33 | outbox_init | True | `calloc(1, sizeof(struct outbox_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/lib/mqtt_outbox.c:35 | outbox_init | True | `calloc(1, sizeof(struct outbox_list_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/lib/mqtt_outbox.c:44 | outbox_enqueue | True | `calloc(1, sizeof(outbox_item_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/lib/mqtt_outbox.c:52 | outbox_enqueue | True | `heap_caps_malloc(message->len + message->remaining_len, MQTT_OUTBOX_MEMORY)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/lib/platform_esp32_idf.c:25 | MAX_ID_STRING | True | `calloc(1, MAX_ID_STRING)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/mqtt_client.c:415 | 见源码上下文 | True | `calloc(1, sizeof(mqtt_config_storage_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/mqtt_client.c:435 | 见源码上下文 | True | `malloc(buffer_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/mqtt_client.c:483 | 见源码上下文 | True | `malloc(config->session.last_will.msg_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/mqtt_client.c:558 | 见源码上下文 | True | `calloc(1, sizeof(struct ifreq) + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/mqtt_client.c:576 | 见源码上下文 | True | `calloc(client->config->num_alpn_protos + 1, sizeof(*config->broker.verification.alpn_protos))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/mqtt_client.c:604 | 见源码上下文 | True | `malloc(client->config->clientkey_password_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/mqtt_client.c:866 | create_client_data | True | `calloc(1, sizeof(esp_mqtt_error_codes_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/mqtt_client.c:882 | esp_mqtt_client_init | True | `heap_caps_calloc(1, sizeof(struct esp_mqtt_client), #if MQTT_EVENT_QUEUE_SIZE > 1 MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/mqtt_client.c:956 | 见源码上下文 | True | `calloc(1, len + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/mqtt_client.c:1870 | 见源码上下文 | True | `xTaskCreatePinnedToCore(esp_mqtt_task, "mqtt_task", client->config->task_stack, client, client->config->task_prio, &client->task_handle, MQTT_TASK_CORE)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/mqtt/esp-mqtt/mqtt_client.c:1876 | 见源码上下文 | True | `xTaskCreate(esp_mqtt_task, "mqtt_task", client->config->task_stack, client, client->config->task_prio, &client->task_handle)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:18 | ? | True | `heap_caps_malloc_default(size_t size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:19 | ? | True | `heap_caps_realloc_default(void *ptr, size_t size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:20 | ? | True | `heap_caps_aligned_alloc_default(size_t alignment, size_t size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:22 | ? | True | `malloc(size_t size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:24 | malloc | True | `heap_caps_malloc_default(size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:27 | malloc | True | `realloc(void* ptr, size_t size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:29 | realloc | True | `heap_caps_realloc_default(ptr, size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:38 | free | True | `calloc(size_t nmemb, size_t size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:46 | 见源码上下文 | True | `heap_caps_malloc_default(size_bytes)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:62 | _realloc_r | True | `heap_caps_realloc_default(ptr, size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:67 | _malloc_r | True | `heap_caps_malloc_default(size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:72 | _calloc_r | True | `calloc(nmemb, size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:78 | memalign | True | `heap_caps_aligned_alloc_default(alignment, n)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:83 | aligned_alloc | True | `heap_caps_aligned_alloc_default(alignment, n)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/heap.c:93 | 见源码上下文 | True | `heap_caps_aligned_alloc_default(alignment, size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/locks.c:75 | 见源码上下文 | True | `xQueueCreateMutex(mutex_type)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/newlib_init.c:147 | esp_libc_init | True | `malloc(sizeof(char*))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/realpath.c:31 | 见源码上下文 | True | `malloc(PATH_MAX)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/scandir.c:31 | scandir | True | `malloc(array_size * sizeof(struct dirent *))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/scandir.c:45 | 见源码上下文 | True | `malloc(sizeof(struct dirent))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/newlib/src/scandir.c:54 | 见源码上下文 | True | `realloc(entries, array_size * sizeof(struct dirent *))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/nvs_flash/src/nvs_api.cpp:762 | create_iterator | True | `calloc(1, sizeof(nvs_opaque_iterator_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protobuf-c/protobuf-c/protobuf-c/protobuf-c.c:154 | system_alloc | True | `malloc(size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/common/protocomm.c:26 | protocomm_new | True | `calloc(1, sizeof(protocomm_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/common/protocomm.c:99 | 见源码上下文 | True | `calloc(1, sizeof(protocomm_ep_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/common/protocomm.c:311 | 见源码上下文 | True | `calloc(1, sizeof(protocomm_security1_params_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/common/protocomm.c:321 | 见源码上下文 | True | `calloc(1, sizeof(protocomm_security2_params_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/common/protocomm.c:380 | 见源码上下文 | True | `malloc(*outlen)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/crypto/srp6a/esp_srp.c:102 | esp_srp_init | True | `calloc(1, sizeof(esp_srp_handle))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/crypto/srp6a/esp_srp.c:215 | 见源码上下文 | True | `malloc(pad_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/crypto/srp6a/esp_srp.c:468 | 见源码上下文 | True | `malloc(salt_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/crypto/srp6a/esp_srp.c:516 | esp_srp_get_session_key | True | `malloc(len_A)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/crypto/srp6a/esp_srp.c:572 | 见源码上下文 | True | `malloc(SHA512_HASH_SZ)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/crypto/srp6a/esp_srp.c:631 | esp_srp_exchange_proofs | True | `calloc(pad_len, sizeof(char))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/crypto/srp6a/esp_srp_mpi.c:11 | esp_mpi_new | True | `malloc(sizeof (esp_mpi_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/crypto/srp6a/esp_srp_mpi.c:78 | esp_mpi_to_bin | True | `malloc(*len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security0.c:26 | sec0_session_setup | True | `malloc(sizeof(Sec0Payload))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security0.c:27 | sec0_session_setup | True | `malloc(sizeof(S0SessionResp))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security0.c:93 | 见源码上下文 | True | `malloc(*outlen)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security1.c:158 | 见源码上下文 | True | `malloc(sizeof(Sec1Payload))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security1.c:159 | 见源码上下文 | True | `malloc(sizeof(SessionResp1))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security1.c:172 | 见源码上下文 | True | `malloc(PUBLIC_KEY_LEN)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security1.c:248 | 见源码上下文 | True | `malloc(sizeof(mbedtls_ecdh_context))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security1.c:249 | 见源码上下文 | True | `malloc(sizeof(mbedtls_entropy_context))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security1.c:250 | 见源码上下文 | True | `malloc(sizeof(mbedtls_ctr_drbg_context))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security1.c:363 | 见源码上下文 | True | `malloc(sizeof(Sec1Payload))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security1.c:364 | 见源码上下文 | True | `malloc(sizeof(SessionResp0))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security1.c:516 | 见源码上下文 | True | `calloc(1, sizeof(session_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security1.c:557 | 见源码上下文 | True | `malloc(*outlen)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security1.c:622 | 见源码上下文 | True | `malloc(*outlen)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security2.c:174 | 见源码上下文 | True | `malloc(sizeof(Sec2Payload))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security2.c:175 | 见源码上下文 | True | `malloc(sizeof(S2SessionResp0))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security2.c:201 | 见源码上下文 | True | `malloc(cur_session->username_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security2.c:244 | 见源码上下文 | True | `calloc(CLIENT_PROOF_LEN, sizeof(char))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security2.c:297 | 见源码上下文 | True | `malloc(sizeof(Sec2Payload))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security2.c:298 | 见源码上下文 | True | `malloc(sizeof(S2SessionResp1))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security2.c:445 | 见源码上下文 | True | `calloc(1, sizeof(session_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security2.c:493 | 见源码上下文 | True | `malloc(*outlen)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security2.c:543 | 见源码上下文 | True | `malloc(*outlen)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/security/security2.c:612 | 见源码上下文 | True | `malloc(*outlen)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_console.c:137 | 见源码上下文 | True | `malloc(strlen(argv[2]))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_console.c:215 | 见源码上下文 | True | `xTaskCreate(protocomm_console_task, "protocomm_console", config->stack_size, NULL, config->task_priority, &console_task)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_httpd.c:125 | 见源码上下文 | True | `malloc(req->content_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_httpd.c:188 | 见源码上下文 | True | `calloc(1, strlen(ep_name) + 2)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_httpd.c:225 | 见源码上下文 | True | `calloc(1, strlen(ep_name) + 2)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_httpd.c:269 | 见源码上下文 | True | `calloc(1, sizeof(httpd_handle_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:169 | 见源码上下文 | True | `calloc(1, sizeof(struct data_mbuf))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:412 | 见源码上下文 | True | `calloc(BLE_UUID128_VAL_LENGTH, sizeof(uint8_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:435 | 见源码上下文 | True | `calloc(1, data_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:824 | ble_gatt_add_char_dsc | True | `calloc(2, sizeof(struct ble_gatt_dsc_def))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:831 | 见源码上下文 | True | `calloc(1, sizeof(ble_uuid16_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:883 | 见源码上下文 | True | `calloc(1, sizeof(ble_uuid128_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:909 | ble_gatt_add_primary_svcs | True | `calloc((char_count + 1), sizeof(struct ble_gatt_chr_def))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:923 | populate_gatt_db | True | `calloc(2, sizeof(struct ble_gatt_svc_def))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:930 | 见源码上下文 | True | `calloc(1, sizeof(ble_uuid128_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:1097 | 见源码上下文 | True | `calloc(1, sizeof(ble_uuid128_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:1108 | 见源码上下文 | True | `calloc(1, sizeof(struct uuid128_name_buf))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:1130 | 见源码上下文 | True | `malloc((size_t)config->manufacturer_data_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:1140 | 见源码上下文 | True | `calloc(1, sizeof(_protocomm_ble_internal_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:1148 | 见源码上下文 | True | `calloc(endpoint_count, sizeof(protocomm_ble_name_uuid_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:1178 | 见源码上下文 | True | `calloc(1, sizeof(simple_ble_cfg_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/protocomm/src/transports/protocomm_nimble.c:1199 | 见源码上下文 | True | `malloc(BLE_ADDR_LEN)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/pthread/pthread.c:172 | ESP_COMPILER_DIAGNOSTIC_PUSH_IGNORE | True | `malloc(sizeof(esp_pthread_cfg_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/pthread/pthread.c:290 | pthread_create_freertos_task_with_caps | True | `xTaskCreatePinnedToCore(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask, core_id)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/pthread/pthread.c:312 | 见源码上下文 | True | `calloc(1, sizeof(esp_pthread_task_arg_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/pthread/pthread.c:318 | 见源码上下文 | True | `calloc(1, sizeof(esp_pthread_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/pthread/pthread.c:641 | 见源码上下文 | True | `malloc(sizeof(esp_pthread_mutex_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/pthread/pthread_cond_var.c:226 | 见源码上下文 | True | `calloc(1, sizeof(esp_pthread_cond_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/pthread/pthread_local_storage.c:57 | pthread_key_create | True | `malloc(sizeof(key_entry_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/pthread/pthread_local_storage.c:206 | 见源码上下文 | True | `calloc(1, sizeof(values_list_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/pthread/pthread_local_storage.c:231 | 见源码上下文 | True | `malloc(sizeof(value_entry_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/pthread/pthread_rwlock.c:61 | 见源码上下文 | True | `calloc(1, sizeof(esp_pthread_rwlock_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sd_pwr_ctrl/sd_pwr_ctrl_by_on_chip_ldo.c:34 | sd_pwr_ctrl_new_on_chip_ldo | True | `heap_caps_calloc(1, sizeof(sd_pwr_ctrl_drv_t), MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sd_pwr_ctrl/sd_pwr_ctrl_by_on_chip_ldo.c:37 | sd_pwr_ctrl_new_on_chip_ldo | True | `heap_caps_calloc(1, sizeof(sd_pwr_ctrl_ldo_ctx_t), MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_cmd.c:38 | 见源码上下文 | True | `heap_caps_malloc(*actual_size, MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_cmd.c:378 | sdmmc_send_cmd_send_scr | True | `heap_caps_malloc(datalen, MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_cmd.c:383 | 见源码上下文 | True | `heap_caps_get_allocated_size(buf)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_cmd.c:449 | sdmmc_send_cmd_num_of_written_blocks | True | `heap_caps_malloc(datalen, MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_cmd.c:454 | 见源码上下文 | True | `heap_caps_get_allocated_size(buf)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_cmd.c:521 | 见源码上下文 | True | `heap_caps_get_allocated_size(buf)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_cmd.c:682 | 见源码上下文 | True | `heap_caps_get_allocated_size(buf)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_common.c:404 | 见源码上下文 | True | `heap_caps_malloc(SDMMC_IO_BLOCK_SIZE, MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_common.c:409 | 见源码上下文 | True | `heap_caps_get_allocated_size(buf)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_mmc.c:31 | sdmmc_init_mmc_read_ext_csd | True | `heap_caps_malloc(EXT_CSD_MMC_SIZE, MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_mmc.c:36 | 见源码上下文 | True | `heap_caps_get_allocated_size(ext_csd)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_mmc.c:259 | 见源码上下文 | True | `heap_caps_malloc(EXT_CSD_MMC_SIZE, MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_mmc.c:264 | 见源码上下文 | True | `heap_caps_get_allocated_size(ext_csd)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_sd.c:96 | sdmmc_init_sd_ssr | True | `heap_caps_calloc(1, SD_SSR_SIZE, MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_sd.c:101 | 见源码上下文 | True | `heap_caps_get_allocated_size(sd_ssr)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_sd.c:245 | 见源码上下文 | True | `heap_caps_malloc(sizeof(*response), MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_sd.c:367 | read_tuning_block | True | `heap_caps_calloc(1, tuning_block_size, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_sd.c:470 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(*response), MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/sdmmc/sdmmc_sd.c:571 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(*response), MALLOC_CAP_DMA)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spi_flash/esp_flash_spi_init.c:385 | 见源码上下文 | True | `heap_caps_malloc(sizeof(esp_flash_t), caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spi_flash/esp_flash_spi_init.c:391 | 见源码上下文 | True | `heap_caps_malloc(sizeof(memspi_host_inst_t), caps)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spi_flash/flash_mmap.c:79 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(mmap_block_t), MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spi_flash/flash_mmap.c:85 | 见源码上下文 | True | `heap_caps_calloc(1, 1 * sizeof(uint32_t), MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spi_flash/flash_mmap.c:198 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(mmap_block_t), MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spi_flash/flash_mmap.c:204 | 见源码上下文 | True | `heap_caps_calloc(1, block_num * sizeof(uint32_t), MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spi_flash/flash_ops.c:126 | spi_flash_malloc_internal | True | `heap_caps_malloc(size, MALLOC_CAP_8BIT&#124;MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spi_flash/spi_flash_os_func_app.c:247 | 见源码上下文 | True | `heap_caps_malloc(read_chunk_size, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spi_flash/spi_flash_os_func_app.c:332 | 见源码上下文 | True | `heap_caps_malloc(sizeof(app_func_arg_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spiffs/esp_spiffs.c:206 | 见源码上下文 | True | `calloc(1, sizeof(esp_spiffs_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spiffs/esp_spiffs.c:231 | 见源码上下文 | True | `calloc(1, efs->fds_sz)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spiffs/esp_spiffs.c:241 | 见源码上下文 | True | `calloc(1, efs->cache_sz)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spiffs/esp_spiffs.c:250 | 见源码上下文 | True | `calloc(1, work_sz)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spiffs/esp_spiffs.c:257 | 见源码上下文 | True | `calloc(1, sizeof(spiffs))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/spiffs/esp_spiffs.c:693 | vfs_spiffs_opendir | True | `calloc(1, sizeof(vfs_spiffs_dir_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/tcp_transport/transport.c:37 | esp_transport_list_init | True | `calloc(1, sizeof(struct esp_transport_list_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/tcp_transport/transport.c:48 | 见源码上下文 | True | `calloc(1, strlen(scheme) + 1)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/tcp_transport/transport.c:94 | esp_transport_init | True | `calloc(1, sizeof(struct esp_transport_item_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/tcp_transport/transport_internal.c:22 | esp_transport_init_foundation_transport | True | `calloc(1, sizeof(esp_foundation_transport_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/tcp_transport/transport_internal.c:24 | esp_transport_init_foundation_transport | True | `calloc(1, sizeof(struct esp_transport_error_storage))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/tcp_transport/transport_socks_proxy.c:106 | 见源码上下文 | True | `calloc(request_message_len, sizeof(char))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/tcp_transport/transport_socks_proxy.c:111 | 见源码上下文 | True | `calloc(request_message_len, sizeof(char))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/tcp_transport/transport_socks_proxy.c:207 | 见源码上下文 | True | `calloc(1, sizeof(transport_socks_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/tcp_transport/transport_ssl.c:541 | esp_transport_esp_tls_create | True | `calloc(1, sizeof(transport_esp_tls_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/tcp_transport/transport_ws.c:228 | 见源码上下文 | True | `malloc(WS_BUFFER_SIZE)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/tcp_transport/transport_ws.c:664 | 见源码上下文 | True | `malloc(control_frame_buffer_len)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/tcp_transport/transport_ws.c:834 | 见源码上下文 | True | `calloc(1, sizeof(transport_ws_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/tcp_transport/transport_ws.c:849 | 见源码上下文 | True | `malloc(WS_BUFFER_SIZE)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/enum.c:1155 | enum_install | True | `heap_caps_calloc(1, sizeof(enum_driver_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/hcd_dwc.c:1005 | port_obj_alloc | True | `calloc(1, sizeof(port_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/hcd_dwc.c:1006 | port_obj_alloc | True | `malloc(sizeof(usb_dwc_hal_context_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/hcd_dwc.c:1007 | port_obj_alloc | True | `heap_caps_aligned_calloc(USB_DWC_FRAME_LIST_MEM_ALIGN, FRAME_LIST_LEN, sizeof(uint32_t), MALLOC_CAP_DMA &#124; MALLOC_CAP_CACHE_ALIGNED &#124; MALLOC_CAP_INTERNAL)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/hcd_dwc.c:1049 | hcd_install | True | `calloc(1, sizeof(hcd_obj_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/hcd_dwc.c:1410 | hcd_port_init | True | `calloc(port_obj->hal->constant_config.chan_num_total, sizeof(usb_dwc_hal_chan_t*))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/hcd_dwc.c:1625 | 见源码上下文 | True | `calloc(1, sizeof(dma_buffer_block_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/hcd_dwc.c:1631 | 见源码上下文 | True | `heap_caps_aligned_calloc(USB_DWC_QTD_LIST_MEM_ALIGN, desc_list_len * sizeof(usb_dwc_ll_dma_qtd_t), 1, XFER_DESC_LIST_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/hcd_dwc.c:1905 | 见源码上下文 | True | `calloc(1, sizeof(pipe_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/hcd_dwc.c:1906 | 见源码上下文 | True | `calloc(1, sizeof(usb_dwc_hal_chan_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/hub.c:156 | dev_tree_node_new | True | `heap_caps_calloc(1, sizeof(dev_tree_node_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/hub.c:528 | hub_install | True | `heap_caps_calloc(1, sizeof(hub_driver_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usb_host.c:450 | usb_host_install | True | `heap_caps_calloc(1, sizeof(host_lib_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usb_host.c:820 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(client_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usb_host.c:822 | 见源码上下文 | True | `xQueueCreate(client_config->max_num_event_msg, sizeof(usb_host_client_event_msg_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usb_host.c:1214 | 见源码上下文 | True | `heap_caps_malloc(config_desc_full->wTotalLength, MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usb_host.c:1248 | ep_wrapper_alloc | True | `heap_caps_calloc(1, sizeof(ep_wrapper_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usb_host.c:1293 | interface_alloc | True | `heap_caps_calloc(1, sizeof(interface_t) + (sizeof(ep_wrapper_t *) * intf_desc->bNumEndpoints), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usb_phy.c:295 | 见源码上下文 | True | `calloc(1, sizeof(phy_ctrl_obj_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usb_phy.c:353 | 见源码上下文 | True | `calloc(1, sizeof(phy_context_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usb_phy.c:409 | 见源码上下文 | True | `calloc(1, sizeof(usb_phy_ext_io_conf_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usb_private.c:29 | ALIGN_UP | True | `heap_caps_calloc(1, sizeof(urb_t) + (sizeof(usb_isoc_packet_desc_t) * num_isoc_packets), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usb_private.c:38 | ALIGN_UP | True | `heap_caps_malloc(data_buffer_size, DATA_BUFFER_CAPS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usbh.c:319 | endpoint_alloc | True | `heap_caps_calloc(1, sizeof(endpoint_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usbh.c:366 | device_alloc | True | `heap_caps_calloc(1, sizeof(device_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usbh.c:654 | usbh_install | True | `heap_caps_calloc(1, sizeof(usbh_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usbh.c:1258 | usbh_dev_set_desc | True | `heap_caps_malloc(sizeof(usb_device_desc_t), MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usbh.c:1299 | usbh_dev_set_config_desc | True | `heap_caps_malloc(config_desc_full->wTotalLength, MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/usb/usbh.c:1341 | usbh_dev_set_str_desc | True | `heap_caps_malloc(str_desc->bLength, MALLOC_CAP_DEFAULT)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs.c:227 | 见源码上下文 | True | `heap_caps_malloc(sizeof(esp_vfs_fs_ops_t), VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs.c:244 | 见源码上下文 | True | `heap_caps_malloc(sizeof(esp_vfs_dir_ops_t), VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs.c:254 | 见源码上下文 | True | `heap_caps_malloc(sizeof(esp_vfs_termios_ops_t), VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs.c:264 | 见源码上下文 | True | `heap_caps_malloc(sizeof(esp_vfs_select_ops_t), VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs.c:296 | 见源码上下文 | True | `heap_caps_malloc(sizeof(esp_vfs_fs_ops_t), VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs.c:343 | 见源码上下文 | True | `heap_caps_malloc(sizeof(esp_vfs_dir_ops_t), VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs.c:361 | 见源码上下文 | True | `heap_caps_malloc(sizeof(esp_vfs_termios_ops_t), VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs.c:378 | 见源码上下文 | True | `heap_caps_malloc(sizeof(esp_vfs_select_ops_t), VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs.c:444 | 见源码上下文 | True | `heap_caps_malloc(sizeof(vfs_entry_t) + base_path_len + 1, VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs.c:1489 | 见源码上下文 | True | `heap_caps_calloc(vfs_count, sizeof(fds_triple_t), VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs.c:1566 | 见源码上下文 | True | `heap_caps_calloc(vfs_count, sizeof(void *), VFS_MALLOC_FLAGS)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs_eventfd.c:111 | 见源码上下文 | True | `malloc(sizeof(event_select_args_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs_eventfd.c:364 | 见源码上下文 | True | `calloc(s_event_size, sizeof(event_context_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/vfs/vfs_semihost.c:323 | 见源码上下文 | True | `calloc(1, sizeof(vfs_semihost_dir_t))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/wear_levelling/wear_levelling.cpp:95 | 见源码上下文 | True | `malloc(sizeof(Partition))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/wear_levelling/wear_levelling.cpp:106 | 见源码上下文 | True | `malloc(sizeof(WL_Ext_Safe))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/wear_levelling/wear_levelling.cpp:115 | 见源码上下文 | True | `malloc(sizeof(WL_Ext_Perf))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/wear_levelling/wear_levelling.cpp:126 | 见源码上下文 | True | `malloc(sizeof(WL_Flash))` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/wear_levelling/WL_Ext_Perf.cpp:44 | 见源码上下文 | True | `malloc(ext_cfg->flash_sector_size)` |
| D:/espidf5.5.5/.espressif/v5.5.5/esp-idf/components/wear_levelling/WL_Flash.cpp:104 | 见源码上下文 | True | `malloc(this->cfg.wl_temp_buff_size)` |
| E:/Lummiss_Plant_Robot/src/demo/components/ambient_led/ambient_led.c:316 | 见源码上下文 | True | `xTaskCreateWithCaps(ambient_led_task, "ambient_led", AMBIENT_LED_TASK_STACK, NULL, AMBIENT_LED_TASK_PRIORITY, &s_task, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/anim_bin_player/anim_bin_player.c:129 | 见源码上下文 | True | `heap_caps_malloc( ANIM_SD_READ_CHUNK, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/anim_bin_player/anim_bin_player.c:136 | 见源码上下文 | True | `heap_caps_malloc(ANIM_BUFFER_SIZE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/anim_bin_player/anim_bin_player.c:297 | 见源码上下文 | True | `heap_caps_malloc( table_size, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/anim_bin_player/anim_bin_player.c:698 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps( player_task, "anim_player", ANIM_TASK_STACK, NULL, ANIM_TASK_PRIORITY, &s_player_task, ANIM_TASK_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/camera_photo/camera_photo.c:225 | 见源码上下文 | True | `xQueueCreate(1, sizeof(photo_request_t))` |
| E:/Lummiss_Plant_Robot/src/demo/components/camera_photo/camera_photo.c:227 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(photo_task, "photo_task", PHOTO_TASK_STACK, NULL, PHOTO_TASK_PRIORITY, &s_photo_task, PHOTO_TASK_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/cloud_mqtt/cloud_mcp.c:187 | 见源码上下文 | True | `heap_caps_calloc( 1, sizeof(*ctx), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/cloud_mqtt/cloud_mcp.c:288 | 见源码上下文 | True | `heap_caps_malloc(sizeof(*job), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/cloud_mqtt/cloud_mcp.c:301 | 见源码上下文 | True | `xTaskCreatePinnedToCore(motion_worker, "mcp_stepper", 3072, job, 4, NULL, 1)` |
| E:/Lummiss_Plant_Robot/src/demo/components/cloud_mqtt/cloud_mcp.c:474 | tool_webrtc_start | True | `heap_caps_calloc( 1, sizeof(*credential), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/cloud_mqtt/cloud_mcp.c:862 | 见源码上下文 | True | `heap_caps_malloc(sizeof(*msg), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/cloud_mqtt/cloud_mcp.c:882 | 见源码上下文 | True | `heap_caps_malloc(CLOUD_MCP_QUEUE_DEPTH * sizeof(mcp_message_t), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/cloud_mqtt/cloud_mcp.c:885 | 见源码上下文 | True | `xQueueCreateStatic(CLOUD_MCP_QUEUE_DEPTH, sizeof(mcp_message_t), s_queue_storage, &s_queue_control)` |
| E:/Lummiss_Plant_Robot/src/demo/components/cloud_mqtt/cloud_mcp.c:901 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(cloud_mcp_task, "cloud_mcp", CLOUD_MCP_TASK_STACK, NULL, CLOUD_MCP_TASK_PRIO, NULL, CLOUD_MCP_TASK_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/cloud_udp/cloud_udp.c:245 | gcm_contexts_init | True | `heap_caps_calloc(1, sizeof(*s_gcm_tx), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/cloud_udp/cloud_udp.c:247 | gcm_contexts_init | True | `heap_caps_calloc(1, sizeof(*s_gcm_rx), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/cloud_udp/cloud_udp.c:606 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(cloud_udp_rx_task, "cloud_udp_rx", CLOUD_UDP_TASK_STACK, NULL, CLOUD_UDP_TASK_PRIO, &s_rx_task, CLOUD_UDP_TASK_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:288 | 见源码上下文 | True | `heap_caps_malloc(SDSPI_BLOCK_BUF_SIZE, MALLOC_CAP_DMA)` |
| E:/Lummiss_Plant_Robot/src/demo/components/esp_driver_sdspi/src/sdspi_host.c:483 | sdspi_host_init_device | True | `malloc(sizeof(slot_info_t))` |
| E:/Lummiss_Plant_Robot/src/demo/components/home_info/home_info.c:368 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps( home_info_task, "home_info", HOME_INFO_TASK_STACK, NULL, HOME_INFO_TASK_PRIORITY, NULL, HOME_INFO_TASK_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/mech_button/mech_button.c:179 | 见源码上下文 | True | `xTaskCreateWithCaps(button_task, "button_task", BUTTON_TASK_STACK, NULL, BUTTON_TASK_PRIORITY, &s_task, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/person_detect/person_detect.cpp:473 | 见源码上下文 | True | `heap_caps_malloc( MODEL_RGB565_BUFFER_SIZE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/person_detect/person_detect.cpp:503 | 见源码上下文 | True | `xQueueCreate(1, sizeof(queued_frame_t))` |
| E:/Lummiss_Plant_Robot/src/demo/components/person_detect/person_detect.cpp:511 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps( person_detect_task, "person_detect", TASK_STACK_SIZE, nullptr, TASK_PRIORITY, &s_task, TASK_CORE, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/sd_card/sd_card_spi.c:282 | 见源码上下文 | True | `calloc(1, sizeof(*card))` |
| E:/Lummiss_Plant_Robot/src/demo/components/touch_key/touch_key.c:293 | 见源码上下文 | True | `xTaskCreateWithCaps(touch_key_task, "touch_key_task", TOUCH_KEY_TASK_STACK, NULL, TOUCH_KEY_TASK_PRIORITY, &s_task, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/dma2d_yuv.c:152 | 见源码上下文 | True | `heap_caps_aligned_calloc( DMA2D_YUV_CACHE_ALIGN, 1, DMA2D_YUV_CACHE_ALIGN, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/dma2d_yuv.c:155 | 见源码上下文 | True | `heap_caps_aligned_calloc( DMA2D_YUV_CACHE_ALIGN, 1, DMA2D_YUV_CACHE_ALIGN, MALLOC_CAP_DMA &#124; MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/dma2d_yuv.c:158 | 见源码上下文 | True | `heap_caps_calloc( 1, SIZEOF_DMA2D_TRANS_T, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:1988 | video_codec_task | True | `esp_h264_aligned_calloc( VIDEO_WS_ALIGN, 1, VIDEO_H264_INPUT_SIZE, &aligned_input_size, ESP_H264_MEM_SPIRAM)` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2054 | 见源码上下文 | True | `xQueueCreate(VIDEO_OUT_SLOT_COUNT, sizeof(unsigned))` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2055 | 见源码上下文 | True | `xQueueCreate(VIDEO_OUT_SLOT_COUNT, sizeof(unsigned))` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2057 | 见源码上下文 | True | `heap_caps_calloc( VIDEO_WS_AUDIO_QUEUE_DEPTH, sizeof(video_agent_audio_packet_t), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2062 | 见源码上下文 | True | `xQueueCreateStatic( VIDEO_WS_AUDIO_QUEUE_DEPTH, sizeof(video_agent_audio_packet_t), s_agent_audio_queue_storage, &s_agent_audio_queue_control)` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2082 | 见源码上下文 | True | `esp_h264_aligned_calloc( VIDEO_WS_ALIGN, 1, VIDEO_H264_SLOT_SIZE, &actual_size, ESP_H264_MEM_SPIRAM)` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2098 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(video_upload_task, "video_upload", VIDEO_TASK_STACK, NULL, VIDEO_UPLOAD_PRIORITY, NULL, VIDEO_UPLOAD_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2113 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(video_report_task, "video_report", 4096, NULL, VIDEO_REPORT_PRIORITY, NULL, VIDEO_REPORT_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2673 | 见源码上下文 | True | `xQueueCreate(VIDEO_CTRL_QUEUE_DEPTH, sizeof(video_ctrl_cmd_t))` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2683 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps( video_ctrl_task, "video_ctrl", 3072, NULL, 5, &s_ctrl_task, VIDEO_UPLOAD_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2703 | 见源码上下文 | True | `xQueueCreate(VIDEO_SLOT_COUNT, sizeof(unsigned))` |
| E:/Lummiss_Plant_Robot/src/demo/components/video_streamer/video_streamer.c:2763 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps( video_codec_task, "video_codec", VIDEO_TASK_STACK, NULL, VIDEO_CODEC_PRIORITY, &s_codec_task, VIDEO_CODEC_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/rtc_heap_diag.c:79 | rtc_heap_diag_init | True | `heap_caps_calloc(CAP_RECORD_MAX, sizeof(*s_caps), MALLOC_CAP_RTCRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/rtc_heap_diag.c:82 | rtc_heap_diag_init | True | `heap_caps_register_failed_alloc_callback(allocation_failed)` |
| E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/rtc_heap_diag.c:104 | rtc_heap_diag_peer_begin | True | `heap_caps_calloc(PEER_TRACE_RECORD_MAX, sizeof(*s_records), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/webrtc_whip.c:634 | begin_peer | True | `heap_caps_malloc(WHIP_SDP_CAPACITY, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/webrtc_whip.c:822 | 见源码上下文 | True | `heap_caps_malloc(url_capacity, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/webrtc_whip.c:824 | 见源码上下文 | True | `heap_caps_calloc(1, WHIP_CREDENTIAL_BODY_CAPACITY, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/webrtc_whip.c:1145 | 见源码上下文 | True | `heap_caps_malloc(WHIP_SDP_CAPACITY, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/webrtc_whip.c:1223 | 见源码上下文 | True | `heap_caps_calloc(64, sizeof(*snapshot), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/webrtc_whip.c:1456 | 见源码上下文 | True | `heap_caps_malloc(2U * sizeof(control_command_t), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/webrtc_whip.c:1459 | 见源码上下文 | True | `xQueueCreateStatic(2, sizeof(control_command_t), s_commands_storage, &s_commands_control)` |
| E:/Lummiss_Plant_Robot/src/demo/components/webrtc_whip/webrtc_whip.c:1533 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(controller_task, "webrtc_whip", 12288, NULL, 4, &s_task, 0, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/wake_word.c:229 | 见源码上下文 | True | `heap_caps_malloc( s_ww.feed_chunk_size * sizeof(int16_t), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/wake_word.c:233 | 见源码上下文 | True | `heap_caps_malloc( s_ww.preroll_capacity * sizeof(int16_t), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/wake_word.c:249 | 见源码上下文 | True | `xTaskCreatePinnedToCore( detection_task, "wake_detect", 4096, NULL, 11, &s_ww.detection_task, 1)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:430 | 见源码上下文 | True | `heap_caps_malloc(available * sizeof(int16_t), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1051 | audio_work_buffers_init | True | `heap_caps_malloc( capture_raw_size, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1055 | audio_work_buffers_init | True | `heap_caps_malloc( (size_t)s_audio.encoder_input_size, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1058 | audio_work_buffers_init | True | `heap_caps_malloc( (size_t)s_audio.encoder_output_size, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1065 | audio_work_buffers_init | True | `heap_caps_malloc( XIAOZHI_PCM_BUFFER_BYTES, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1819 | 见源码上下文 | True | `heap_caps_calloc( XIAOZHI_PLAYBACK_QUEUE_DEPTH, sizeof(xiaozhi_opus_packet_t), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1828 | 见源码上下文 | True | `xQueueCreateStatic( XIAOZHI_PLAYBACK_QUEUE_DEPTH, sizeof(xiaozhi_opus_packet_t), s_playback_queue_storage, &s_playback_queue_control)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1839 | 见源码上下文 | True | `xQueueCreateStatic( XIAOZHI_PCM_POOL_SIZE, sizeof(uint8_t *), s_pcm_free_queue_storage, &s_pcm_free_queue_control)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1844 | 见源码上下文 | True | `xQueueCreateStatic( XIAOZHI_PCM_QUEUE_DEPTH, sizeof(xiaozhi_pcm_frame_t), s_pcm_ready_queue_storage, &s_pcm_ready_queue_control)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1863 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps( output_task, "xiaozhi_spk", XIAOZHI_OUTPUT_STACK_BYTES, NULL, XIAOZHI_OUTPUT_PRIORITY, &s_output_task, XIAOZHI_OUTPUT_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1868 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps( wake_process_task, "wake_process", XIAOZHI_WAKE_PROCESS_STACK_BYTES, NULL, XIAOZHI_WAKE_PROCESS_PRIORITY, &s_wake_process_task, XIAOZHI_WAKE_PROCESS_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1876 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps( decode_task, "xiaozhi_dec", XIAOZHI_DECODE_STACK_BYTES, NULL, XIAOZHI_DECODE_PRIORITY, &s_decode_task, XIAOZHI_DECODE_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:1882 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps( capture_task, "xiaozhi_mic", XIAOZHI_CAPTURE_STACK_BYTES, NULL, XIAOZHI_CAPTURE_PRIORITY, &s_capture_task, XIAOZHI_CAPTURE_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:2308 | 见源码上下文 | True | `heap_caps_malloc( AUDIO_HW_TEST_MIC_BYTES, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_DMA)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:2310 | 见源码上下文 | True | `heap_caps_malloc( AUDIO_HW_TEST_TONE_BYTES, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:2342 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps( audio_hardware_test_mic_task, "audio_test_mic", AUDIO_HW_TEST_STACK_BYTES, NULL, AUDIO_HW_TEST_TASK_PRIORITY, &s_hardware_test_mic_task, AUDIO_HW_TEST_TASK_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:2348 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps( audio_hardware_test_spk_task, "audio_test_spk", AUDIO_HW_TEST_STACK_BYTES, NULL, AUDIO_HW_TEST_TASK_PRIORITY, &s_hardware_test_spk_task, AUDIO_HW_TEST_TASK_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:2425 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps( service_task, "xiaozhi_service", XIAOZHI_SERVICE_STACK_BYTES, NULL, XIAOZHI_SERVICE_PRIORITY, &s_service_task, XIAOZHI_CAPTURE_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/components/xiaozhi_audio/xiaozhi_audio.c:2449 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(expression_selftest_task, "expr_selftest", 3072, NULL, 2, NULL, tskNO_AFFINITY, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/main/camera_driver.c:509 | camera_frame_copy_pool_init | True | `heap_caps_malloc( CAMERA_HANDOFF_COPY_SIZE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/main/camera_driver.c:869 | 见源码上下文 | True | `xTaskCreatePinnedToCore(usb_events_task, "usb_events", 4096, NULL, CAMERA_USB_EVENTS_PRIORITY, &s_usb_events_task, CAMERA_USB_CORE)` |
| E:/Lummiss_Plant_Robot/src/demo/main/camera_driver.c:999 | 见源码上下文 | True | `xQueueCreate( CAMERA_HANDOFF_QUEUE_LENGTH, sizeof(camera_frame_handoff_item_t))` |
| E:/Lummiss_Plant_Robot/src/demo/main/camera_driver.c:1003 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(camera_handoff_task, "camera_handoff", CAMERA_HANDOFF_TASK_STACK, NULL, CAMERA_HANDOFF_TASK_PRIORITY, &handoff_task, CAMERA_USB_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/main/display_driver.c:271 | lcd_show_raw_test_pattern | True | `heap_caps_malloc( LCD_PANEL_H_RES * sizeof(uint16_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_DMA)` |
| E:/Lummiss_Plant_Robot/src/demo/main/expression_manager.c:185 | 见源码上下文 | True | `heap_caps_malloc(EXPRESSION_QUEUE_LEN * sizeof(expression_event_t), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/main/expression_manager.c:188 | 见源码上下文 | True | `xQueueCreateStatic(EXPRESSION_QUEUE_LEN, sizeof(expression_event_t), s_queue_storage, &s_queue_control)` |
| E:/Lummiss_Plant_Robot/src/demo/main/main.c:309 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(camera_task, "camera_task", 8192, NULL, 7, NULL, APP_USB_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/main/main.c:674 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(ui_task, "ui_task", 8192, NULL, 6, NULL, APP_UI_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/main/main.c:679 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(ui_task, "ui_task", 8192, NULL, 6, NULL, APP_UI_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/main/main.c:703 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(camera_task, "camera_task", 8192, NULL, 7, NULL, APP_USB_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/main/main.c:820 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(ui_task, "ui_task", 8192, NULL, 6, NULL, APP_UI_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/main/main.c:827 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(camera_task, "camera_task", 8192, NULL, 7, NULL, APP_USB_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/main/main.c:840 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(idle_stable_diag_task, "idle_stable_diag", 3072, NULL, 2, NULL, APP_UI_CORE, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/main/peripheral_test.c:136 | 见源码上下文 | True | `xTaskCreate(limit_input_task, "limit_input", LIMIT_TASK_STACK, NULL, LIMIT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/main/peripheral_test.c:393 | 见源码上下文 | True | `xTaskCreatePinnedToCore( stepper_test_task, "stepper_test", STEPPER_TEST_TASK_STACK, NULL, STEPPER_TEST_TASK_PRIORITY, NULL, STEPPER_TEST_CORE)` |
| E:/Lummiss_Plant_Robot/src/demo/main/peripheral_test.c:451 | 见源码上下文 | True | `xTaskCreate(peripheral_test_task, "periph_test", PERIPH_TEST_TASK_STACK, NULL, PERIPH_TEST_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/main/peripheral_test.c:463 | 见源码上下文 | True | `xTaskCreate(quiet_check_task, "periph_quiet", PERIPH_QUIET_TASK_STACK, NULL, PERIPH_TEST_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/main/screen_carousel.c:247 | 见源码上下文 | True | `xTaskCreatePinnedToCore( carousel_task, "screen_carousel", CAROUSEL_TASK_STACK, NULL, CAROUSEL_TASK_PRIORITY, NULL, CAROUSEL_TASK_CORE)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/base/dl_fft2r_fc32_ansi.c:188 | dl_gen_rfft_table_f32 | True | `heap_caps_aligned_alloc(16, fft_point * sizeof(float), caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/base/dl_fft2r_fc32_ansi.c:220 | 见源码上下文 | True | `heap_caps_malloc(2 * count * sizeof(uint16_t), caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/base/dl_fft2r_fc32_ansi.c:244 | dl_gen_fft2r_table_f32 | True | `heap_caps_aligned_alloc(16, fft_point * sizeof(float), caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/base/dl_fft2r_sc16_ansi.c:457 | dl_gen_fft_table_sc16 | True | `heap_caps_aligned_alloc(16, fft_point * sizeof(int16_t), caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/base/dl_fft2r_sc16_ansi.c:473 | dl_gen_rfft_table_s16 | True | `heap_caps_aligned_alloc(16, fft_point * sizeof(int16_t), caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/base/dl_fft2r_sc16_dif_ansi.c:5 | dl_gen_dif_fft_table | True | `heap_caps_aligned_alloc(16, N * 2 * sizeof(int16_t), caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/base/dl_fft2r_sc16_dif_ansi.c:31 | dl_gen_dif_rfft_table | True | `heap_caps_aligned_alloc(16, N * sizeof(int16_t), caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/base/dl_fft4r_fc32_ansi.c:238 | 见源码上下文 | True | `heap_caps_malloc(2 * count * sizeof(uint16_t), caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/base/dl_fft4r_fc32_ansi.c:266 | dl_gen_fft4r_table_f32 | True | `heap_caps_aligned_alloc(16, fft_point * sizeof(float) * 2, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/base/dl_fft_table.c:72 | 见源码上下文 | True | `heap_caps_malloc(sizeof(dl_fft_table_node_t), MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/dl_fft_f32.c:17 | 见源码上下文 | True | `heap_caps_malloc(sizeof(dl_fft_f32_t), caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/dl_fft_s16.c:17 | 见源码上下文 | True | `heap_caps_malloc(sizeof(dl_fft_s16_t), caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/dl_rfft_f32.c:17 | 见源码上下文 | True | `heap_caps_malloc(sizeof(dl_fft_f32_t), caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__dl_fft/dl_rfft_s16.c:10 | dl_rfft_s16_init | True | `heap_caps_malloc(sizeof(dl_fft_s16_t), caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__eppp_link/eppp_eth.c:197 | eppp_eth_init | False | `calloc(1, sizeof(struct eppp_handle))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__eppp_link/eppp_link.c:308 | 见源码上下文 | True | `xTaskCreate(ppp_task, "ppp connect", config->task.stack_size, netif, config->task.priority, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__eppp_link/eppp_sdio.c:69 | eppp_sdio_init | False | `calloc(1, sizeof(struct eppp_sdio))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__eppp_link/eppp_sdio_host.c:114 | eppp_sdio_host_init | False | `malloc(sizeof(sdmmc_card_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__eppp_link/eppp_spi.c:81 | transmit_generic | False | `malloc(batch)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__eppp_link/eppp_spi.c:441 | eppp_spi_init | False | `calloc(1, sizeof(struct eppp_spi))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__eppp_link/eppp_spi.c:448 | eppp_spi_init | False | `xQueueCreate(CONFIG_EPPP_LINK_PACKET_QUEUE_SIZE, sizeof(struct packet))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__eppp_link/eppp_uart.c:265 | eppp_uart_init | True | `calloc(1, sizeof(struct eppp_uart))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/audio/common/dl_audio_common.cpp:12 | win_func_init | True | `heap_caps_malloc(sizeof(float) * win_len, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/audio/common/dl_audio_common.cpp:187 | mel_filter_init | True | `malloc(sizeof(float) * (nfilter + 2))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/audio/common/dl_audio_common.cpp:188 | mel_filter_init | True | `malloc(sizeof(float) * (feat_width))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/audio/common/dl_audio_common.cpp:191 | mel_filter_init | True | `heap_caps_malloc(sizeof(mel_filter_t), caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/audio/common/dl_audio_common.cpp:192 | mel_filter_init | True | `heap_caps_malloc(sizeof(float) * feat_width * 2, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/audio/common/dl_audio_common.cpp:193 | mel_filter_init | True | `heap_caps_malloc(sizeof(int) * nfilter * 2, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/audio/common/dl_audio_wav.cpp:39 | decode_wav | True | `calloc(1, sizeof(dl_audio_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/audio/common/dl_audio_wav.cpp:107 | 见源码上下文 | True | `malloc(audio_data_len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/audio/speech_features/dl_mfcc.cpp:14 | gen_dct_matrix | True | `heap_caps_aligned_alloc(16, sizeof(float) * num_rows * num_cols, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/audio/speech_features/dl_mfcc.cpp:41 | gen_lifter_coeffs | True | `heap_caps_aligned_alloc(16, sizeof(float) * len, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/dl/module/src/dl_module_base.cpp:69 | 见源码上下文 | True | `xTaskCreateStaticPinnedToCore(DualCoreWorkerTask, i == 0 ? "dl_mc0" : "dl_mc1", kWorkerStackBytes, &runtime.task_args[i], kWorkerDefaultPriority, runtime.task_stacks[i], &runtime.task_buffers[i], static_cast<BaseType_t>(i))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/dl/module/src/dl_module_base.cpp:103 | 见源码上下文 | True | `malloc(sizeof(char) * length)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/dl/tool/src/dl_tool.cpp:164 | malloc_aligned | True | `heap_caps_aligned_alloc(16, size, caps &#124; MALLOC_CAP_SIMD)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/dl/tool/src/dl_tool.cpp:166 | malloc_aligned | True | `heap_caps_aligned_alloc(16, size, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/dl/tool/src/dl_tool.cpp:187 | calloc_aligned | True | `heap_caps_aligned_calloc(16, n, size, caps &#124; MALLOC_CAP_SIMD)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/dl/tool/src/dl_tool.cpp:189 | calloc_aligned | True | `heap_caps_aligned_calloc(16, n, size, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/fbs_loader/src/fbs_loader.cpp:405 | 见源码上下文 | True | `malloc(sizeof(esp_partition_mmap_handle_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/fbs_loader/src/fbs_loader.cpp:765 | 见源码上下文 | True | `malloc(chunk_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_bmp.cpp:158 | 见源码上下文 | True | `heap_caps_malloc(img.height * img.width * 3, MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_bmp.cpp:205 | 见源码上下文 | True | `heap_caps_malloc(img.height * img.width * channel, MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_draw.cpp:68 | 见源码上下文 | True | `heap_caps_malloc(row_len, MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_jpeg.cpp:62 | 见源码上下文 | True | `heap_caps_aligned_alloc(16, img.bytes(), MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_jpeg.cpp:121 | 见源码上下文 | True | `heap_caps_malloc(out_buf_len, MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_jpeg.cpp:158 | 见源码上下文 | True | `heap_caps_malloc(img.height * img.width * 3, MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_jpeg.cpp:257 | 见源码上下文 | True | `heap_caps_aligned_calloc(alignment, 1, out_buf_len, MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_jpeg.cpp:332 | 见源码上下文 | True | `heap_caps_aligned_calloc(alignment, 1, out_buf_len, MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_jpeg.cpp:374 | 见源码上下文 | True | `heap_caps_malloc(img.height * img.width * 3, MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_jpeg.cpp:389 | 见源码上下文 | True | `heap_caps_malloc(img.height * img.width * 2, MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_jpeg.cpp:445 | 见源码上下文 | True | `heap_caps_malloc(img.data_len, MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_process.cpp:516 | 见源码上下文 | True | `heap_caps_malloc(dst_width * sizeof(int), MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_SIMD)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_process.cpp:517 | 见源码上下文 | True | `heap_caps_malloc(dst_height * sizeof(int), MALLOC_CAP_DEFAULT &#124; MALLOC_CAP_SIMD)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_process.cpp:538 | 见源码上下文 | True | `heap_caps_malloc(dst_width * sizeof(int), MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_process.cpp:539 | 见源码上下文 | True | `heap_caps_malloc(dst_width * sizeof(int), MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_process.cpp:540 | 见源码上下文 | True | `heap_caps_malloc(dst_height * sizeof(int), MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/image/dl_image_process.cpp:541 | 见源码上下文 | True | `heap_caps_malloc(dst_height * sizeof(int), MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/recognition/dl_recognition_database.cpp:102 | 见源码上下文 | True | `heap_caps_malloc(m_meta.feat_len * sizeof(float), MALLOC_CAP_SPIRAM)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dl/vision/recognition/dl_recognition_database.cpp:130 | 见源码上下文 | True | `heap_caps_malloc(m_meta.feat_len * sizeof(float), MALLOC_CAP_SPIRAM)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/azure_board_apps/apps/3d_graphics/main/3d_graphics_demo.cpp:222 | app_main | False | `xTaskCreate(draw_3d_image_task, "draw_3d_image", 2048, &image, 4, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:332 | kalman_filter_calibration | False | `malloc(state_vectors_size * sizeof(float))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:456 | 见源码上下文 | False | `malloc(state_vectors_size * sizeof(float))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:597 | 见源码上下文 | False | `xTaskCreate(get_pressure_task, "get_pressure_task", 2048 * 4, &init_pressure, 4, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:602 | 见源码上下文 | False | `xTaskCreate(kalman_filter_task, "kalman_filter_task", 2048 * 4, &image, 5, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:603 | 见源码上下文 | False | `xTaskCreate(save_state_vectors_task, "save_state_vectors", 2048, ekf13, 6, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/lyrat_board_app/main/audio_amp_main.c:192 | 见源码上下文 | False | `xTaskCreate(buttons_process_task, "buttons_process_task", 4096, NULL, 4, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/lyrat_board_app/main/audio_amp_main.c:197 | 见源码上下文 | False | `xTaskCreate(audio_read_task, "audio_read_task", 4096, NULL, 7, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/lyrat_board_app/main/audio_amp_main.c:370 | app_main | False | `xQueueCreate(10, sizeof(int))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/lyrat_board_app/main/audio_amp_main.c:373 | app_main | False | `xTaskCreate(audio_process_task, "audio_process_task", 4096, NULL, 6, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/m5stack_core_s3/apps/3d_graphics/main/3d_graphics_demo.cpp:131 | app_main | False | `xTaskCreate(draw_3d_image_task, "draw_3d_image", 16384, &image, 3, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/m5stack_core_s3/apps/kalman_filter/main/3d_kalman_demo.cpp:138 | 见源码上下文 | False | `malloc(length + 4)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/m5stack_core_s3/apps/kalman_filter/main/3d_kalman_demo.cpp:198 | app_init | False | `malloc(CUBE_EDGES * sizeof(lv_obj_t *))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/m5stack_core_s3/apps/kalman_filter/main/3d_kalman_demo.cpp:199 | app_init | False | `malloc(CUBE_EDGES * 2 * sizeof(lv_point_precise_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/m5stack_core_s3/apps/kalman_filter/main/3d_kalman_demo.cpp:434 | app_main | False | `xTaskCreate(draw_3d_image_task, "draw_3d_image", 16384, &image, 4, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/spectrum_box_lite/main/main.c:258 | app_main | False | `xTaskCreatePinnedToCore(&microphone_read_task, "Microphone read Task", 8 * 1024, NULL, 3, NULL, 0)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/applications/spectrum_box_lite/main/main.c:264 | 见源码上下文 | False | `xTaskCreatePinnedToCore(&image_display_task, "Draw task", 10 * 1024, NULL, 5, NULL, 1)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/1fa4e38f/lyrat_board_app/main/audio_amp_main.c:192 | 见源码上下文 | False | `xTaskCreate(buttons_process_task, "buttons_process_task", 4096, NULL, 4, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/1fa4e38f/lyrat_board_app/main/audio_amp_main.c:197 | 见源码上下文 | False | `xTaskCreate(audio_read_task, "audio_read_task", 4096, NULL, 7, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/1fa4e38f/lyrat_board_app/main/audio_amp_main.c:370 | app_main | False | `xQueueCreate(10, sizeof(int))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/1fa4e38f/lyrat_board_app/main/audio_amp_main.c:373 | app_main | False | `xTaskCreate(audio_process_task, "audio_process_task", 4096, NULL, 6, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/azure_board_apps/apps/3d_graphics/main/3d_graphics_demo.cpp:222 | app_main | False | `xTaskCreate(draw_3d_image_task, "draw_3d_image", 2048, &image, 4, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:332 | kalman_filter_calibration | False | `malloc(state_vectors_size * sizeof(float))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:456 | 见源码上下文 | False | `malloc(state_vectors_size * sizeof(float))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:597 | 见源码上下文 | False | `xTaskCreate(get_pressure_task, "get_pressure_task", 2048 * 4, &init_pressure, 4, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:602 | 见源码上下文 | False | `xTaskCreate(kalman_filter_task, "kalman_filter_task", 2048 * 4, &image, 5, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:603 | 见源码上下文 | False | `xTaskCreate(save_state_vectors_task, "save_state_vectors", 2048, ekf13, 6, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/lyrat_board_app/main/audio_amp_main.c:192 | 见源码上下文 | False | `xTaskCreate(buttons_process_task, "buttons_process_task", 4096, NULL, 4, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/lyrat_board_app/main/audio_amp_main.c:197 | 见源码上下文 | False | `xTaskCreate(audio_read_task, "audio_read_task", 4096, NULL, 7, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/lyrat_board_app/main/audio_amp_main.c:370 | app_main | False | `xQueueCreate(10, sizeof(int))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/lyrat_board_app/main/audio_amp_main.c:373 | app_main | False | `xTaskCreate(audio_process_task, "audio_process_task", 4096, NULL, 6, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/m5stack_core_s3/apps/3d_graphics/main/3d_graphics_demo.cpp:131 | app_main | False | `xTaskCreate(draw_3d_image_task, "draw_3d_image", 16384, &image, 3, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/m5stack_core_s3/apps/kalman_filter/main/3d_kalman_demo.cpp:138 | 见源码上下文 | False | `malloc(length + 4)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/m5stack_core_s3/apps/kalman_filter/main/3d_kalman_demo.cpp:198 | app_init | False | `malloc(CUBE_EDGES * sizeof(lv_obj_t *))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/m5stack_core_s3/apps/kalman_filter/main/3d_kalman_demo.cpp:199 | app_init | False | `malloc(CUBE_EDGES * 2 * sizeof(lv_point_precise_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/m5stack_core_s3/apps/kalman_filter/main/3d_kalman_demo.cpp:434 | app_main | False | `xTaskCreate(draw_3d_image_task, "draw_3d_image", 16384, &image, 4, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/spectrum_box_lite/main/main.c:258 | app_main | False | `xTaskCreatePinnedToCore(&microphone_read_task, "Microphone read Task", 8 * 1024, NULL, 3, NULL, 0)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/b00f000e/applications/spectrum_box_lite/main/main.c:264 | 见源码上下文 | False | `xTaskCreatePinnedToCore(&image_display_task, "Draw task", 10 * 1024, NULL, 5, NULL, 1)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/d1468452/apps/3d_graphics/main/3d_graphics_demo.cpp:222 | app_main | False | `xTaskCreate(draw_3d_image_task, "draw_3d_image", 2048, &image, 4, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/d1468452/apps/kalman_filter/main/kalman_filter_demo.cpp:332 | kalman_filter_calibration | False | `malloc(state_vectors_size * sizeof(float))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/d1468452/apps/kalman_filter/main/kalman_filter_demo.cpp:456 | 见源码上下文 | False | `malloc(state_vectors_size * sizeof(float))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/d1468452/apps/kalman_filter/main/kalman_filter_demo.cpp:597 | 见源码上下文 | False | `xTaskCreate(get_pressure_task, "get_pressure_task", 2048 * 4, &init_pressure, 4, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/d1468452/apps/kalman_filter/main/kalman_filter_demo.cpp:602 | 见源码上下文 | False | `xTaskCreate(kalman_filter_task, "kalman_filter_task", 2048 * 4, &image, 5, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/d1468452/apps/kalman_filter/main/kalman_filter_demo.cpp:603 | 见源码上下文 | False | `xTaskCreate(save_state_vectors_task, "save_state_vectors", 2048, ekf13, 6, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/f9c2d4b3/azure_board_apps/apps/3d_graphics/main/3d_graphics_demo.cpp:222 | app_main | False | `xTaskCreate(draw_3d_image_task, "draw_3d_image", 2048, &image, 4, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/f9c2d4b3/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:332 | kalman_filter_calibration | False | `malloc(state_vectors_size * sizeof(float))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/f9c2d4b3/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:456 | 见源码上下文 | False | `malloc(state_vectors_size * sizeof(float))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/f9c2d4b3/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:597 | 见源码上下文 | False | `xTaskCreate(get_pressure_task, "get_pressure_task", 2048 * 4, &init_pressure, 4, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/f9c2d4b3/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:602 | 见源码上下文 | False | `xTaskCreate(kalman_filter_task, "kalman_filter_task", 2048 * 4, &image, 5, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/external_examples/f9c2d4b3/azure_board_apps/apps/kalman_filter/main/kalman_filter_demo.cpp:603 | 见源码上下文 | False | `xTaskCreate(save_state_vectors_task, "save_state_vectors", 2048, ekf13, 6, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/modules/common/include/dsp_tests.h:35 | 见源码上下文 | False | `malloc(size_)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/modules/fft/float/dsps_fft2r_fc32_ansi.c:67 | 见源码上下文 | True | `malloc(table_size * sizeof(float))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/modules/fft/float/dsps_fft2r_fc32_ansi.c:81 | 见源码上下文 | True | `malloc(2 * dsps_fft2r_rev_tables_fc32_size[pow - 4] * sizeof(uint16_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/modules/fft/float/dsps_fft4r_fc32_ansi.c:54 | 见源码上下文 | True | `malloc(max_fft_size * sizeof(float) * 4)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/modules/fft/float/dsps_fft4r_fc32_ansi.c:66 | 见源码上下文 | True | `malloc(2 * dsps_fft4r_rev_tables_fc32_size[pow - 2] * sizeof(uint16_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/modules/fir/fixed/dsps_firmr_init_s16.c:33 | 见源码上下文 | True | `malloc((fir->delay_size + 4) * sizeof(int16_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/modules/fir/float/dsps_fir_init_f32.c:26 | 见源码上下文 | True | `malloc((coeffs_len + 4) * sizeof(float))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/modules/fir/float/dsps_firmr_init_f32.c:29 | 见源码上下文 | True | `malloc((fir->delay_size + 4) * sizeof(float))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/modules/fir/resampler/dsps_resampler_mr.c:31 | 见源码上下文 | True | `malloc(sizeof(fir_f32_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/modules/fir/resampler/dsps_resampler_mr.c:36 | 见源码上下文 | True | `malloc(sizeof(fir_s16_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/modules/support/cplx_gen/dsps_cplx_gen_init.c:56 | 见源码上下文 | True | `malloc(cplx_gen->lut_len * sizeof(int16_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-dsp/modules/support/cplx_gen/dsps_cplx_gen_init.c:65 | 见源码上下文 | True | `malloc(cplx_gen->lut_len * sizeof(float))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/esp_mn_speech_commands.c:18 | _esp_mn_calloc_ | True | `heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/esp_mn_speech_commands.c:20 | _esp_mn_calloc_ | True | `calloc(n, size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/esp_mn_speech_commands.c:23 | _esp_mn_calloc_ | True | `calloc(n, size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/esp_process_sdkconfig.c:939 | 见源码上下文 | True | `calloc(command_str_len + 1, 1)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:47 | 见源码上下文 | True | `malloc((size + 1) * sizeof(char))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:66 | get_wake_words_from_info | True | `malloc(info_len + 1)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:78 | 见源码上下文 | True | `malloc(word_len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:83 | 见源码上下文 | True | `realloc(wake_words, word_len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:99 | srmodel_list_alloc | True | `malloc(sizeof(srmodel_list_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:151 | 见源码上下文 | True | `malloc(models->num * sizeof(char *))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:153 | 见源码上下文 | True | `calloc(MODEL_NAME_MAX_LENGTH, sizeof(char))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:266 | 见源码上下文 | True | `malloc(sizeof(srmodel_data_t *) * models->num)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:267 | 见源码上下文 | True | `malloc(sizeof(char *) * models->num)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:268 | 见源码上下文 | True | `malloc(sizeof(char *) * models->num)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:271 | 见源码上下文 | True | `malloc(sizeof(srmodel_data_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:274 | 见源码上下文 | True | `malloc((strlen(data) + 1) * sizeof(char))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:281 | 见源码上下文 | True | `malloc(sizeof(char *) * file_num)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:282 | 见源码上下文 | True | `malloc(sizeof(void *) * file_num)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:283 | 见源码上下文 | True | `malloc(sizeof(int) * file_num)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:329 | 见源码上下文 | True | `malloc(sizeof(esp_partition_mmap_handle_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:340 | 见源码上下文 | True | `malloc(sizeof(spi_flash_mmap_handle_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:401 | 见源码上下文 | True | `calloc(len, sizeof(char))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:460 | 见源码上下文 | True | `malloc(models->num * sizeof(char *))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:461 | 见源码上下文 | True | `malloc(models->num * sizeof(char *))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:463 | 见源码上下文 | True | `calloc(MODEL_NAME_MAX_LENGTH, sizeof(char))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp-sr/src/model_path.c:481 | 见源码上下文 | True | `malloc(file_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/audio_codec_sw_vol.c:129 | audio_codec_new_sw_vol | True | `calloc(1, sizeof(audio_vol_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/aw88298/aw88298.c:342 | 见源码上下文 | True | `calloc(1, sizeof(audio_codec_aw88298_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/cjc8910/cjc8910.c:358 | 见源码上下文 | False | `calloc(1, sizeof(audio_codec_cjc8910_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/dummy/dummy_codec.c:124 | 见源码上下文 | False | `calloc(1, sizeof(dummy_codec_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/es7210/es7210.c:594 | 见源码上下文 | True | `calloc(1, sizeof(audio_codec_es7210_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/es7243/es7243.c:206 | 见源码上下文 | True | `calloc(1, sizeof(audio_codec_es7243_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/es7243e/es7243e.c:251 | 见源码上下文 | True | `calloc(1, sizeof(audio_codec_es7243e_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/es8156/es8156.c:271 | 见源码上下文 | True | `calloc(1, sizeof(audio_codec_es8156_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/es8311/es8311.c:737 | 见源码上下文 | True | `calloc(1, sizeof(audio_codec_es8311_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/es8374/es8374.c:739 | 见源码上下文 | True | `calloc(1, sizeof(audio_codec_es8374_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/es8388/es8388.c:405 | 见源码上下文 | True | `calloc(1, sizeof(audio_codec_es8388_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/es8389/es8389.c:741 | 见源码上下文 | True | `calloc(1, sizeof(audio_codec_es8389_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/tas5805m/tas5805m.c:258 | 见源码上下文 | True | `calloc(1, sizeof(audio_codec_tas5805m_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/zl38063/example_apps/tw_ldcfg.c:72 | 见源码上下文 | False | `malloc(len * sizeof(dataArr))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/zl38063/example_apps/tw_ldfwcfg.c:74 | 见源码上下文 | False | `malloc(len * sizeof(dataArr))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/device/zl38063/zl38063.c:286 | 见源码上下文 | False | `calloc(1, sizeof(audio_codec_zl38063_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/esp_codec_dev.c:79 | _get_default_vol_curve | True | `malloc(2 * sizeof(esp_codec_dev_vol_map_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/esp_codec_dev.c:133 | 见源码上下文 | True | `calloc(1, sizeof(codec_dev_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/esp_codec_dev.c:299 | 见源码上下文 | True | `realloc(dev->vol_curve.vol_map, size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_ctrl_i2c.c:206 | 见源码上下文 | True | `calloc(1, sizeof(i2c_ctrl_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_ctrl_spi.c:142 | audio_codec_new_spi_ctrl | True | `calloc(1, sizeof(spi_ctrl_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_data_adc.c:576 | audio_codec_new_adc_data | False | `calloc(1, sizeof(adc_data_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_data_adc.c:643 | 见源码上下文 | False | `calloc(1, adc_data->raw_read_buf_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_data_adc.c:653 | 见源码上下文 | False | `calloc(parsed_buf_count, sizeof(codec_adc_parsed_data_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c:114 | add_to_keeper | True | `calloc(1, sizeof(i2s_data_keep_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_data_i2s.c:762 | audio_codec_new_i2s_data | True | `calloc(1, sizeof(i2s_data_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_codec_dev/platform/audio_codec_gpio.c:49 | audio_codec_new_gpio | True | `calloc(1, sizeof(audio_codec_gpio_if_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_dual_hw.c:312 | enc_open | True | `esp_h264_intr_alloc(0, h264_frame_isr, (void *)hw_hd, &hw_hd->intr_hd)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_dual_hw.c:420 | 见源码上下文 | True | `esp_h264_calloc_prefer(1, sizeof(esp_h264_hw_handle_t), &actual_size, ESP_H264_MEM_SPIRAM, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_dual_hw.c:446 | 见源码上下文 | True | `esp_h264_aligned_calloc(16, 1, esp_h264_enc_hw_max_db_tmp_buffer_size(width), &actual_size, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_dual_hw.c:450 | 见源码上下文 | True | `esp_h264_aligned_calloc(16, 1, sizeof(h264_dma_desc_t), &actual_size, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_dual_hw.c:453 | 见源码上下文 | True | `esp_h264_aligned_calloc(16, 1, sizeof(h264_dma_desc_t), &actual_size, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_dual_hw.c:455 | 见源码上下文 | True | `esp_h264_aligned_calloc(16, 1, sizeof(h264_dma_desc_t), &actual_size, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_hw_param.c:379 | esp_h264_enc_hw_new_param | True | `esp_h264_calloc_prefer(1, sizeof(esp_h264_param_t), &actual_size, ESP_H264_MEM_SPIRAM, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_hw_param.c:406 | 见源码上下文 | True | `esp_h264_aligned_calloc(4, 1, param->nal_buf_len, &actual_size, ESP_H264_MEM_SPIRAM)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_hw_param.c:420 | 见源码上下文 | True | `esp_h264_aligned_malloc( 16, 1, ref_requested_bytes, &actual_size, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_hw_param.c:434 | 见源码上下文 | True | `esp_h264_malloc_prefer(1, max_db_buffer_size(param->mb_width, param->mb_height), &actual_size, ESP_H264_MEM_INTERNAL, ESP_H264_MEM_SPIRAM)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_hw_param.c:438 | 见源码上下文 | True | `esp_h264_aligned_calloc(16, 1, sizeof(h264_dma_desc_t), &actual_size, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_hw_param.c:441 | 见源码上下文 | True | `esp_h264_aligned_calloc(16, 1, sizeof(h264_dma_desc_t), &actual_size, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_hw_param.c:444 | 见源码上下文 | True | `esp_h264_aligned_calloc(16, 1, sizeof(h264_dma_desc_t), &actual_size, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_single_hw.c:280 | enc_open | True | `esp_h264_intr_alloc(0, h264_gop_isr, (void *)hw_hd, &hw_hd->intr_hd)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_single_hw.c:383 | 见源码上下文 | True | `esp_h264_calloc_prefer(1, sizeof(esp_h264_hw_handle_t), &actual_size, ESP_H264_MEM_SPIRAM, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_single_hw.c:407 | 见源码上下文 | True | `esp_h264_aligned_calloc(16, 1, esp_h264_enc_hw_max_db_tmp_buffer_size(cfg->res.width), &actual_size, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_single_hw.c:412 | 见源码上下文 | True | `esp_h264_aligned_calloc(16, 1, sizeof(h264_dma_desc_t), &actual_size, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_single_hw.c:415 | 见源码上下文 | True | `esp_h264_aligned_calloc(16, 1, sizeof(h264_dma_desc_t), &actual_size, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/esp_h264_enc_single_hw.c:417 | 见源码上下文 | True | `esp_h264_aligned_calloc(16, 1, sizeof(h264_dma_desc_t), &actual_size, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/hw/src/h264_rc.c:84 | 见源码上下文 | True | `esp_h264_aligned_calloc(4, 1, sizeof(esp_h264_rc_t), &actual_size, ESP_H264_MEM_SPIRAM)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/inc/esp_h264_intr_alloc.h:14 | ? | False | `esp_h264_intr_alloc(flags, handler, arg, ret_handle)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/include/esp_h264_alloc.h:40 | ? | False | `esp_h264_aligned_malloc(uint32_t alignment, uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/include/esp_h264_alloc.h:58 | ? | False | `esp_h264_aligned_calloc(uint32_t alignment, uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/include/esp_h264_alloc.h:73 | ? | False | `esp_h264_malloc_prefer(uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps1, uint32_t caps2)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/include/esp_h264_alloc.h:88 | ? | False | `esp_h264_calloc_prefer(uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps1, uint32_t caps2)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc.c:11 | ? | True | `esp_h264_aligned_malloc(uint32_t alignment, uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc.c:18 | esp_h264_aligned_malloc | True | `heap_caps_aligned_alloc((size_t)alignment, (size_t) * actual_size, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc.c:25 | 见源码上下文 | True | `esp_h264_aligned_calloc(uint32_t alignment, uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc.c:32 | esp_h264_aligned_calloc | True | `heap_caps_aligned_calloc((size_t)alignment, 1, (size_t) * actual_size, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc.c:39 | 见源码上下文 | True | `esp_h264_malloc_prefer(uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps1, uint32_t caps2)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc.c:41 | esp_h264_malloc_prefer | True | `esp_h264_aligned_malloc(4, n, size, actual_size, caps1)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc.c:45 | 见源码上下文 | True | `esp_h264_aligned_malloc(4, n, size, actual_size, caps2)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc.c:48 | 见源码上下文 | True | `esp_h264_calloc_prefer(uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps1, uint32_t caps2)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc.c:50 | esp_h264_calloc_prefer | True | `esp_h264_aligned_calloc(4, n, size, actual_size, caps1)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc.c:54 | 见源码上下文 | True | `esp_h264_aligned_calloc(4, n, size, actual_size, caps2)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc_less_than_5_3.c:9 | ? | False | `esp_h264_aligned_malloc(uint32_t alignment, uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc_less_than_5_3.c:12 | esp_h264_aligned_malloc | False | `heap_caps_aligned_alloc((size_t)alignment, (size_t) * actual_size, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc_less_than_5_3.c:15 | esp_h264_aligned_malloc | False | `esp_h264_aligned_calloc(uint32_t alignment, uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc_less_than_5_3.c:18 | esp_h264_aligned_calloc | False | `heap_caps_aligned_calloc((size_t)alignment, (size_t)n, (size_t)size, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc_less_than_5_3.c:22 | esp_h264_aligned_calloc | False | `esp_h264_malloc_prefer(uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps1, uint32_t caps2)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc_less_than_5_3.c:25 | esp_h264_malloc_prefer | False | `heap_caps_malloc_prefer((size_t) * actual_size, 2, caps1, caps2)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc_less_than_5_3.c:29 | esp_h264_malloc_prefer | False | `esp_h264_calloc_prefer(uint32_t n, uint32_t size, uint32_t *actual_size, uint32_t caps1, uint32_t caps2)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/port/src/esp_h264_alloc_less_than_5_3.c:32 | esp_h264_calloc_prefer | False | `heap_caps_calloc_prefer((size_t)n, (size_t)size, 2, caps1, caps2)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/sw/src/esp_h264_dec_sw.c:104 | esp_h264_dec_sw_new | True | `esp_h264_calloc_prefer(1, sizeof(esp_h264_dec_sw_handle_t), &actual_size, ESP_H264_MEM_SPIRAM, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/sw/src/esp_h264_enc_single_sw.c:195 | esp_h264_enc_sw_new | True | `esp_h264_calloc_prefer(1, sizeof(esp_h264_enc_sw_handle_t), &actual_size, ESP_H264_MEM_SPIRAM, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/sw/src/esp_h264_enc_single_sw.c:229 | 见源码上下文 | True | `esp_h264_aligned_calloc(16, 1, yuv_cache_size, &actual_size, ESP_H264_MEM_SPIRAM)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/sw/src/esp_h264_enc_single_sw.c:231 | 见源码上下文 | True | `esp_h264_aligned_calloc(16, 1, yuv_cache_size, &actual_size, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_h264/sw/src/esp_h264_enc_sw_param.c:120 | esp_h264_enc_sw_new_param | True | `esp_h264_calloc_prefer(1, sizeof(esp_h264_enc_sw_param_t), &actual_size, ESP_H264_MEM_SPIRAM, ESP_H264_MEM_INTERNAL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/common/protobuf-c/protobuf-c/protobuf-c.c:154 | system_alloc | True | `malloc(size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/common/protobuf-c/t/generated-code/test-generated-code.c:39 | main | False | `malloc (size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/common/protobuf-c/t/generated-code2/test-generated-code2.c:70 | TEST_VERSUS_STATIC_ARRAY | False | `malloc (siz1)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/common/protobuf-c/t/generated-code2/test-generated-code2.c:1882 | test_field_merge | False | `malloc (msg_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/common/protobuf-c/t/generated-code2/test-generated-code2.c:1929 | test_submessage_merge | False | `malloc (size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/common/protobuf-c/t/generated-code2/test-generated-code2.c:1942 | test_submessage_merge | False | `malloc (size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/common/protobuf-c/t/generated-code2/test-generated-code2.c:1962 | test_alloc | False | `malloc (size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/common/protobuf-c/t/generated-code2/test-generated-code2.c:1996 | test_free | False | `malloc (len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/common/protobuf-c/t/generated-code2/test-generated-code2.c:2042 | test_free_unpacked_input_check_for_null_repeated_field | False | `calloc(1, foo__test_mess__descriptor.sizeof_message)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/common/utils/esp_hosted_cli.c:54 | task_dump_cli_handler | True | `calloc(1, num_of_tasks * sizeof(TaskStatus_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/common/utils/esp_hosted_cli.c:80 | cpu_dump_cli_handler | True | `calloc(1, 2 * 1024)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/common/utils/esp_hosted_cli.c:121 | 见源码上下文 | True | `malloc(heap_trace_records * sizeof(heap_trace_record_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c:904 | 见源码上下文 | True | `heap_caps_malloc(packet_size, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/host/drivers/transport/sdio/sdio_drv.c:1186 | 见源码上下文 | True | `heap_caps_malloc( buf_handle->payload_len, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/host/drivers/transport/uart/uart_drv.c:420 | 见源码上下文 | False | `malloc(MAX_UART_BUFFER_SIZE)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/host/port/esp/freertos/src/port_esp_hosted_host_os.c:87 | hosted_malloc | True | `malloc(size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/host/port/esp/freertos/src/port_esp_hosted_host_os.c:131 | hosted_malloc_align | True | `heap_caps_aligned_alloc(align, size, MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_DMA &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/host/port/esp/freertos/src/port_esp_hosted_host_os.c:163 | 见源码上下文 | True | `xTaskCreate((void (*)(void *))start_routine, tname, tstack_size, sr_arg, tprio, thread_handle)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/host/port/esp/freertos/src/port_esp_hosted_host_os.c:257 | 见源码上下文 | True | `xQueueCreate(qnum_elem, qitem_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/host/port/esp/freertos/src/port_esp_hosted_host_spi_hd.c:360 | hosted_spi_hd_init | False | `calloc(1, sizeof(spi_hd_ctx_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/common/protobuf-c/protobuf-c/protobuf-c.c:154 | system_alloc | False | `malloc(size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/common/protobuf-c/t/generated-code/test-generated-code.c:39 | main | False | `malloc (size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/common/protobuf-c/t/generated-code2/test-generated-code2.c:70 | TEST_VERSUS_STATIC_ARRAY | False | `malloc (siz1)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/common/protobuf-c/t/generated-code2/test-generated-code2.c:1882 | test_field_merge | False | `malloc (msg_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/common/protobuf-c/t/generated-code2/test-generated-code2.c:1929 | test_submessage_merge | False | `malloc (size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/common/protobuf-c/t/generated-code2/test-generated-code2.c:1942 | test_submessage_merge | False | `malloc (size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/common/protobuf-c/t/generated-code2/test-generated-code2.c:1962 | test_alloc | False | `malloc (size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/common/protobuf-c/t/generated-code2/test-generated-code2.c:1996 | test_free | False | `malloc (len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/common/protobuf-c/t/generated-code2/test-generated-code2.c:2042 | test_free_unpacked_input_check_for_null_repeated_field | False | `calloc(1, foo__test_mess__descriptor.sizeof_message)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/common/utils/esp_hosted_cli.c:54 | task_dump_cli_handler | False | `calloc(1, num_of_tasks * sizeof(TaskStatus_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/common/utils/esp_hosted_cli.c:80 | cpu_dump_cli_handler | False | `calloc(1, 2 * 1024)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/common/utils/esp_hosted_cli.c:121 | 见源码上下文 | False | `malloc(heap_trace_records * sizeof(heap_trace_record_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/esp_hosted_coprocessor.c:774 | 见源码上下文 | False | `xTaskCreate(power_save_alert_task, "ps_alert_task", 3072, (void *)ESP_POWER_SAVE_ON, tskIDLE_PRIORITY + 5, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/esp_hosted_coprocessor.c:784 | 见源码上下文 | False | `xTaskCreate(power_save_alert_task, "ps_alert_task", 3072, (void *)ESP_POWER_SAVE_OFF, tskIDLE_PRIORITY + 5, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/esp_hosted_coprocessor.c:1002 | 见源码上下文 | False | `xTaskCreate(recv_task , "recv_task" , CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL , CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/esp_hosted_coprocessor.c:1041 | 见源码上下文 | False | `xQueueCreate(TO_HOST_QUEUE_SIZE*3, sizeof(uint8_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/esp_hosted_coprocessor.c:1044 | 见源码上下文 | False | `xQueueCreate(TO_HOST_QUEUE_SIZE, sizeof(interface_buffer_handle_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/esp_hosted_coprocessor.c:1048 | 见源码上下文 | False | `xTaskCreate(send_task , "send_task" , CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL , CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/esp_hosted_coprocessor.c:1067 | 见源码上下文 | False | `xTaskCreate(host_reset_task, "host_reset_task" , CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL , CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/mempool.h:47 | ? | False | `calloc(x,y)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/mempool.h:48 | ? | False | `malloc(x)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/mempool.h:49 | ? | False | `heap_caps_malloc(x, MALLOC_CAP_DMA)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/protocomm_pserial.c:94 | compose_tlv | False | `calloc(1, buf_len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/protocomm_pserial.c:236 | 见源码上下文 | False | `malloc(len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/protocomm_pserial.c:320 | 见源码上下文 | False | `malloc(sizeof(struct pserial_config))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/protocomm_pserial.c:327 | 见源码上下文 | False | `xQueueCreate(REQ_Q_MAX, sizeof(serial_arg_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/protocomm_pserial.c:333 | 见源码上下文 | False | `xTaskCreate(pserial_task, "pserial_task", ESP_HOSTED_PROTOBUF_TASK_STACK_SIZE, (void *) pc, CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/sdio_slave_api.c:379 | 见源码上下文 | False | `xQueueCreate(SDIO_NUM_RX_BUFFERS, sizeof(interface_buffer_handle_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/sdio_slave_api.c:428 | 见源码上下文 | False | `xTaskCreate(sdio_rx_task, "sdio_rx_task" , CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL, CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/sdio_slave_api.c:433 | 见源码上下文 | False | `xTaskCreate(sdio_tx_done_task, "sdio_tx_done_task" , CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL, CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/sdio_slave_api.c:821 | 见源码上下文 | False | `xTaskCreate(sdio_reset_task, "sdio_reset", CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, handle, CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/sdio_slave_api.c:902 | 见源码上下文 | False | `xTaskCreate(sdio_deinit_task, "sdio_deinit", CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, handle, CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_bt.c:46 | host_rcv_pkt | False | `malloc(len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_bt.c:161 | ble_hs_hci_rx_evt | False | `malloc(len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_bt.c:175 | ble_hs_rx_data | False | `malloc(len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:265 | 见源码上下文 | False | `calloc(1,sizeof(RpcRespOTABegin))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:312 | 见源码上下文 | False | `calloc(1,sizeof(RpcRespOTAWrite))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:349 | 见源码上下文 | False | `calloc(1,sizeof(RpcRespOTAEnd))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:391 | 见源码上下文 | False | `calloc(1,sizeof(RpcRespOTAActivate))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:482 | 见源码上下文 | False | `calloc(1,sizeof(vendor_ie_data_t)+p_vid->payload.len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:500 | 见源码上下文 | False | `calloc(1,sizeof(RpcRespSetSoftAPVendorSpecificIE))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:1977 | req_wifi_scan_get_ap_record | False | `calloc(1, sizeof(wifi_ap_record_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:2018 | req_wifi_scan_get_ap_records | False | `calloc(number, sizeof(wifi_ap_record_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:2037 | 见源码上下文 | False | `calloc(number, sizeof(WifiApRecord *))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:2276 | req_wifi_ap_get_sta_list | False | `calloc(ESP_WIFI_MAX_CONN_NUM, sizeof(WifiStaInfo *))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:2727 | 见源码上下文 | False | `malloc(g_ca_cert_len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:2767 | 见源码上下文 | False | `malloc(g_client_cert_len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:2776 | 见源码上下文 | False | `malloc(g_private_key_len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:2786 | 见源码上下文 | False | `malloc(g_private_key_passwd_len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:3810 | data_transfer_handler | False | `calloc(1, sizeof(Rpc))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:3852 | 见源码上下文 | False | `calloc(1, *outlen)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:3879 | rpc_evt_ESPInit | False | `calloc(1,sizeof(RpcEventESPInit))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:3896 | rpc_evt_heartbeat | False | `calloc(1,sizeof(RpcEventHeartbeat))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:4120 | rpc_evt_itwt_suspend | False | `calloc(num_elements, sizeof(p_a->actual_suspend_time_ms[0]))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:4333 | rpc_evt_handler | False | `calloc(1, sizeof(Rpc))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.c:4427 | 见源码上下文 | False | `calloc(1, *outlen)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.h:33 | 见源码上下文 | False | `calloc(1,sizeof(NtFy_TyPe))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.h:51 | 见源码上下文 | False | `calloc(1, sizeof(RspTyPe))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.h:67 | 见源码上下文 | False | `calloc(1, sizeof(RspTyPe))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.h:89 | RPC_ALLOC_ELEMENT | False | `calloc(1, sizeof(TyPe))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.h:100 | NTFY_ALLOC_ELEMENT | False | `calloc(1, sizeof(TyPe))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.h:113 | 见源码上下文 | False | `calloc(1, num)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.h:155 | 见源码上下文 | False | `calloc(1, num)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/slave_control.h:184 | 见源码上下文 | False | `calloc(1, num)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/spi_hd_slave_api.c:652 | 见源码上下文 | False | `xQueueCreate(SPI_HD_QUEUE_SIZE, sizeof(interface_buffer_handle_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/spi_hd_slave_api.c:656 | 见源码上下文 | False | `xTaskCreate(spi_hd_rx_task, "spi_hd_rx_task" , CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL, CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/spi_hd_slave_api.c:661 | 见源码上下文 | False | `xTaskCreate(spi_hd_tx_done_task, "spi_hd_tx_done_task" , CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL, CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/spi_hd_slave_api.c:665 | 见源码上下文 | False | `xTaskCreate(flow_ctrl_task, "flow_ctrl_task" , CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL , CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/spi_slave_api.c:769 | 见源码上下文 | False | `xQueueCreate(SPI_RX_QUEUE_SIZE, sizeof(interface_buffer_handle_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/spi_slave_api.c:772 | 见源码上下文 | False | `xQueueCreate(SPI_TX_QUEUE_SIZE, sizeof(interface_buffer_handle_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/spi_slave_api.c:776 | 见源码上下文 | False | `xTaskCreate(spi_transaction_post_process_task , "spi_post_process_task" , CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL, CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/stats.c:102 | log_real_time_stats | False | `malloc(sizeof(TaskStatus_t) * start_array_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/stats.c:118 | 见源码上下文 | False | `malloc(sizeof(TaskStatus_t) * end_array_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/stats.c:348 | 见源码上下文 | False | `xTaskCreate(raw_tp_tx_task , "raw_tp_tx_task", CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL , CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/stats.c:358 | create_debugging_tasks | False | `xTaskCreate(log_runtime_stats_task, "log_runtime_stats_task", CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL, 1, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/uart_slave_api.c:252 | 见源码上下文 | False | `malloc(BUFFER_SIZE)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/uart_slave_api.c:530 | 见源码上下文 | False | `xQueueCreate(HOSTED_UART_RX_QUEUE_SIZE, sizeof(interface_buffer_handle_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/uart_slave_api.c:535 | 见源码上下文 | False | `xTaskCreate(uart_rx_task, "uart_rx_task" , CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL, CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/main/uart_slave_api.c:539 | 见源码上下文 | False | `xTaskCreate(flow_ctrl_task, "flow_ctrl_task" , CONFIG_ESP_HOSTED_DEFAULT_TASK_STACK_SIZE, NULL , CONFIG_ESP_HOSTED_DEFAULT_TASK_PRIORITY, NULL)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/managed_components/esp-qa__wifi-cmd/src/wifi_cmd.c:230 | 见源码上下文 | False | `malloc(sizeof(wifi_sta_list_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/managed_components/esp-qa__wifi-cmd/src/wifi_handler.c:136 | 见源码上下文 | False | `malloc(sta_number * sizeof(wifi_ap_record_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/managed_components/esp-qa__wifi-cmd/src/wifi_init.c:226 | 见源码上下文 | False | `malloc(sizeof(wifi_init_config_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/managed_components/esp-qa__wifi-cmd/src/wifi_init.c:498 | 见源码上下文 | False | `malloc(sizeof(wifi_init_config_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/managed_components/esp-qa__wifi-cmd/src/wifi_offchannel.c:145 | 见源码上下文 | False | `malloc(sizeof(wifi_action_tx_req_t) + data_len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/managed_components/esp-qa__wifi-cmd/src/wifi_stats_cmd.c:203 | wifi_cmd_get_tx_statistics | False | `malloc(sizeof(esp_test_tx_tb_statistics_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/managed_components/esp-qa__wifi-cmd/src/wifi_stats_cmd.c:208 | 见源码上下文 | False | `malloc(sizeof(esp_test_tx_statistics_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/managed_components/esp-qa__wifi-cmd/src/wifi_stats_cmd.c:214 | 见源码上下文 | False | `malloc(TEST_TX_FAIL_MAX * sizeof(esp_test_tx_fail_statistics_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/managed_components/esp-qa__wifi-cmd/src/wifi_stats_cmd.c:611 | print_rx_mu_statistics | False | `malloc(sizeof(esp_test_rx_mu_statistics_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/managed_components/esp-qa__wifi-cmd/src/wifi_stats_cmd.c:676 | wifi_cmd_get_rx_statistics | False | `malloc(sizeof(esp_test_rx_statistics_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/managed_components/espressif__iperf/iperf.c:132 | iperf_start_report | False | `xTaskCreatePinnedToCore(iperf_report_task, IPERF_REPORT_TASK_NAME, IPERF_REPORT_TASK_STACK, NULL, IPERF_REPORT_TASK_PRIORITY, NULL, NUMBER_OF_CORES - 1)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/managed_components/espressif__iperf/iperf.c:631 | 见源码上下文 | False | `malloc(s_iperf_ctrl.buffer_len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_hosted/slave/managed_components/espressif__iperf/iperf.c:637 | 见源码上下文 | False | `xTaskCreatePinnedToCore(iperf_task_traffic, IPERF_TRAFFIC_TASK_NAME, IPERF_TRAFFIC_TASK_STACK, NULL, IPERF_TRAFFIC_TASK_PRIORITY, NULL, NUMBER_OF_CORES - 1)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_libsrtp/libsrtp/crypto/kernel/alloc.c:78 | 见源码上下文 | True | `calloc(1, size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/common/ppa/lcd_ppa.c:43 | lvgl_port_ppa_create | True | `malloc(sizeof(lvgl_port_ppa_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/common/ppa/lcd_ppa.c:59 | 见源码上下文 | True | `heap_caps_aligned_calloc(CONFIG_CACHE_L2_CACHE_LINE_SIZE, ppa_ctx->buffer_size, sizeof(uint8_t), buffer_caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port.c:79 | 见源码上下文 | True | `xTaskCreateWithCaps(lvgl_port_task, "taskLVGL", cfg->task_stack, NULL, cfg->task_priority, &lvgl_port_ctx.lvgl_task, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port.c:82 | 见源码上下文 | True | `xTaskCreatePinnedToCoreWithCaps(lvgl_port_task, "taskLVGL", cfg->task_stack, NULL, cfg->task_priority, &lvgl_port_ctx.lvgl_task, cfg->task_affinity, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_button.c:53 | lvgl_port_add_navigation_buttons | False | `malloc(sizeof(lvgl_port_nav_btns_ctx_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:276 | lvgl_port_add_disp_priv | True | `heap_caps_malloc(sizeof(lvgl_port_display_ctx_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:279 | lvgl_port_add_disp_priv | True | `malloc(sizeof(lvgl_port_display_ctx_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:337 | 见源码上下文 | True | `heap_caps_malloc(disp_cfg->trans_size * sizeof(lv_color_t), MALLOC_CAP_DMA)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:348 | 见源码上下文 | True | `heap_caps_malloc(buffer_size * sizeof(lv_color_t), buff_caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:351 | 见源码上下文 | True | `heap_caps_malloc(buffer_size * sizeof(lv_color_t), buff_caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_disp.c:356 | 见源码上下文 | True | `malloc(sizeof(lv_disp_draw_buf_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_knob.c:49 | lvgl_port_add_encoder | False | `malloc(sizeof(lvgl_port_encoder_ctx_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_touch.c:45 | lvgl_port_add_touch | False | `malloc(sizeof(lvgl_port_touch_ctx_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_usbhid.c:182 | 见源码上下文 | False | `xQueueCreate(10, sizeof(lvgl_port_usb_hid_event_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl8/esp_lvgl_port_usbhid.c:183 | 见源码上下文 | False | `xTaskCreate(&lvgl_port_usb_hid_task, "hid_task", 4 * 1024, &lvgl_hid_ctx, 2, &lvgl_hid_ctx.task)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c:87 | 见源码上下文 | False | `xTaskCreateWithCaps(lvgl_port_task, "taskLVGL", cfg->task_stack, xTaskGetCurrentTaskHandle(), cfg->task_priority, &lvgl_port_ctx.lvgl_task, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c:90 | 见源码上下文 | False | `xTaskCreatePinnedToCoreWithCaps(lvgl_port_task, "taskLVGL", cfg->task_stack, xTaskGetCurrentTaskHandle(), cfg->task_priority, &lvgl_port_ctx.lvgl_task, cfg->task_affinity, caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_button.c:53 | lvgl_port_add_navigation_buttons | False | `malloc(sizeof(lvgl_port_nav_btns_ctx_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c:328 | 见源码上下文 | False | `heap_caps_malloc(sizeof(lvgl_port_display_ctx_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c:331 | 见源码上下文 | False | `malloc(sizeof(lvgl_port_display_ctx_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c:385 | 见源码上下文 | False | `heap_caps_aligned_alloc(CONFIG_LV_DRAW_BUF_ALIGN, buffer_size * color_bytes, buff_caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c:388 | 见源码上下文 | False | `heap_caps_aligned_alloc(CONFIG_LV_DRAW_BUF_ALIGN, buffer_size * color_bytes, buff_caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c:422 | 见源码上下文 | False | `heap_caps_malloc(buffer_size, buff_caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c:474 | 见源码上下文 | False | `heap_caps_malloc(buffer_size * color_bytes, buff_caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_knob.c:49 | lvgl_port_add_encoder | False | `malloc(sizeof(lvgl_port_encoder_ctx_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_touch.c:50 | lvgl_port_add_touch | False | `malloc(sizeof(lvgl_port_touch_ctx_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_usbhid.c:196 | 见源码上下文 | False | `xQueueCreate(10, sizeof(lvgl_port_usb_hid_event_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_usbhid.c:197 | 见源码上下文 | False | `xTaskCreate(&lvgl_port_usb_hid_task, "hid_task", 4 * 1024, &lvgl_hid_ctx, 2, &lvgl_hid_ctx.task)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_new_jpeg/test_app/main/test_decoder.c:39 | 见源码上下文 | False | `calloc(1, sizeof(jpeg_dec_io_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_new_jpeg/test_app/main/test_decoder.c:46 | 见源码上下文 | False | `calloc(1, sizeof(jpeg_dec_header_info_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_new_jpeg/test_app/main/test_decoder.c:122 | 见源码上下文 | False | `calloc(1, sizeof(jpeg_dec_io_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_new_jpeg/test_app/main/test_decoder.c:129 | 见源码上下文 | False | `calloc(1, sizeof(jpeg_dec_header_info_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_new_jpeg/test_app/main/test_decoder.c:231 | 见源码上下文 | False | `calloc(1, sizeof(jpeg_dec_io_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_new_jpeg/test_app/main/test_decoder.c:238 | 见源码上下文 | False | `calloc(1, sizeof(jpeg_dec_header_info_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_new_jpeg/test_app/main/test_encoder.c:41 | 见源码上下文 | False | `calloc(1, outbuf_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_new_jpeg/test_app/main/test_encoder.c:103 | 见源码上下文 | False | `calloc(1, outbuf_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/dtls_srtp.c:16 | dtls_srtp_selfsign_cert | True | `malloc(DTLS_CERT_PEM_BUF_SIZE)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/dtls_srtp_v6.c:18 | dtls_srtp_selfsign_cert | False | `malloc(DTLS_CERT_PEM_BUF_SIZE)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/esp_peer.c:27 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(peer_wrapper_t), MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/media_lib_weak.c:26 | __attribute__ | True | `heap_caps_malloc(size, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/media_lib_weak.c:31 | media_lib_calloc | True | `heap_caps_calloc(nmemb, size, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/media_lib_weak.c:36 | media_lib_realloc | True | `heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/transport/peer_tls_esp.c:34 | 见源码上下文 | True | `calloc(1, sizeof(peer_tls_esp_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/transport/peer_tls_esp.c:115 | 见源码上下文 | True | `calloc(1, sizeof(peer_tls_esp_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/transport/tcp.c:535 | tcp_connections_open | True | `calloc(1, sizeof(struct tcp_connections_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/transport/tcp.c:553 | 见源码上下文 | True | `calloc(tcp->cfg.max_connections, sizeof(tcp_connection_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/transport/tcp.c:701 | 见源码上下文 | True | `calloc(1, sizeof(tcp_send_item_t) + len + 2)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/transport/tls.c:443 | tls_connections_open | True | `calloc(1, sizeof(struct tls_connections_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/transport/tls.c:461 | 见源码上下文 | True | `calloc(tls->cfg.max_connections, sizeof(tls_connection_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_peer/src/transport/tls.c:615 | 见源码上下文 | True | `calloc(1, sizeof(tls_send_item_t) + len + 2)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_serial_slave_link/essl_sdio.c:112 | essl_sdio_init_dev | True | `heap_caps_malloc(sizeof(essl_sdio_context_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_serial_slave_link/essl_sdio.c:113 | essl_sdio_init_dev | True | `heap_caps_malloc(sizeof(essl_dev_t), MALLOC_CAP_INTERNAL &#124; MALLOC_CAP_8BIT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_serial_slave_link/essl_spi.c:274 | essl_spi_init_dev | True | `calloc(1, sizeof(essl_spi_context_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_serial_slave_link/essl_spi.c:275 | essl_spi_init_dev | True | `calloc(1, sizeof(essl_dev_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_websocket_client/esp_websocket_client.c:178 | 见源码上下文 | True | `calloc(1, client->buffer_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_websocket_client/esp_websocket_client.c:185 | 见源码上下文 | True | `calloc(1, client->buffer_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_websocket_client/esp_websocket_client.c:319 | 见源码上下文 | True | `malloc(needed_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_websocket_client/esp_websocket_client.c:355 | 见源码上下文 | True | `calloc(1, strlen(WS_HTTP_BASIC_AUTH) + n + 1)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_websocket_client/esp_websocket_client.c:788 | esp_websocket_client_init | True | `calloc(1, sizeof(struct esp_websocket_client))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_websocket_client/esp_websocket_client.c:810 | 见源码上下文 | True | `calloc(1, sizeof(struct ifreq) + 1)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_websocket_client/esp_websocket_client.c:823 | 见源码上下文 | True | `calloc(1, sizeof(websocket_config_storage_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_websocket_client/esp_websocket_client.c:898 | 见源码上下文 | True | `malloc(buffer_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_websocket_client/esp_websocket_client.c:902 | 见源码上下文 | True | `malloc(buffer_size)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_websocket_client/esp_websocket_client.c:1045 | 见源码上下文 | True | `malloc(len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_websocket_client/esp_websocket_client.c:1059 | 见源码上下文 | True | `malloc(new_len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_websocket_client/esp_websocket_client.c:1475 | 见源码上下文 | True | `xTaskCreatePinnedToCore(esp_websocket_client_task, client->config->task_name ? client->config->task_name : "websocket_task", client->config->task_stack, client, client->config->task_prio, &client->task_handle, client->config->task_core_id)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__esp_websocket_client/esp_websocket_client.c:1504 | 见源码上下文 | True | `calloc(1, total_len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__led_strip/src/led_strip_rmt_dev.c:145 | 见源码上下文 | True | `calloc(1, sizeof(led_strip_rmt_obj) + led_config->max_leds * bytes_per_pixel)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__led_strip/src/led_strip_rmt_encoder.c:93 | rmt_new_led_strip_encoder | True | `calloc(1, sizeof(rmt_led_strip_encoder_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__led_strip/src/led_strip_spi_dev.c:173 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(led_strip_spi_obj) + led_config->max_leds * bytes_per_pixel * SPI_BYTES_PER_COLOR_BYTE, mem_caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/handlers.c:55 | new_wifi_config | True | `calloc(1, sizeof(network_prov_ctx_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/handlers.c:170 | new_thread_dataset | True | `calloc(1, sizeof(network_prov_ctx_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/manager.c:409 | 见源码上下文 | True | `malloc(sizeof(network_prov_config_handlers_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/manager.c:430 | 见源码上下文 | True | `malloc(sizeof(network_prov_scan_handlers_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/manager.c:453 | 见源码上下文 | True | `malloc(sizeof(network_ctrl_handlers_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/manager.c:916 | 见源码上下文 | True | `calloc(get_count, sizeof(wifi_ap_record_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/manager.c:1453 | 见源码上下文 | True | `malloc(sizeof(otActiveScanResult))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/manager.c:1914 | 见源码上下文 | True | `calloc(1, sizeof(struct network_prov_mgr_ctx))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_config.c:72 | 见源码上下文 | True | `malloc(sizeof(RespGetWifiStatus))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_config.c:89 | 见源码上下文 | True | `malloc(sizeof(WifiConnectedState))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_config.c:140 | 见源码上下文 | True | `calloc(1, sizeof(WifiAttemptFailed))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_config.c:163 | 见源码上下文 | True | `malloc(sizeof(RespGetThreadStatus))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_config.c:179 | 见源码上下文 | True | `malloc(sizeof(ThreadAttachState))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_config.c:189 | 见源码上下文 | True | `malloc(attached->ext_pan_id.len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_config.c:198 | 见源码上下文 | True | `malloc(sizeof(resp_data.conn_info.name))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_config.c:243 | 见源码上下文 | True | `malloc(sizeof(RespSetWifiConfig))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_config.c:296 | 见源码上下文 | True | `malloc(sizeof(RespSetThreadConfig))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_config.c:344 | 见源码上下文 | True | `malloc(sizeof(RespApplyWifiConfig))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_config.c:362 | 见源码上下文 | True | `malloc(sizeof(RespApplyThreadConfig))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_config.c:535 | 见源码上下文 | True | `malloc(*outlen)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_ctrl.c:60 | 见源码上下文 | True | `malloc(sizeof(RespCtrlWifiReset))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_ctrl.c:78 | 见源码上下文 | True | `malloc(sizeof(RespCtrlThreadReset))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_ctrl.c:108 | 见源码上下文 | True | `malloc(sizeof(RespCtrlWifiReprov))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_ctrl.c:126 | 见源码上下文 | True | `malloc(sizeof(RespCtrlThreadReprov))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_ctrl.c:234 | 见源码上下文 | True | `malloc(*outlen)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:73 | 见源码上下文 | True | `malloc(sizeof(RespScanWifiStart))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:102 | 见源码上下文 | True | `malloc(sizeof(RespScanThreadStart))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:144 | 见源码上下文 | True | `malloc(sizeof(RespScanWifiStatus))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:165 | 见源码上下文 | True | `malloc(sizeof(RespScanThreadStatus))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:199 | 见源码上下文 | True | `malloc(sizeof(RespScanWifiResult))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:229 | 见源码上下文 | True | `calloc(req->cmd_scan_wifi_result->count, sizeof(WiFiScanResult *))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:258 | 见源码上下文 | True | `malloc(sizeof(WiFiScanResult))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:282 | 见源码上下文 | True | `malloc(results[i]->bssid.len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:296 | 见源码上下文 | True | `malloc(sizeof(RespScanThreadResult))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:327 | 见源码上下文 | True | `calloc(req->cmd_scan_thread_result->count, sizeof(ThreadScanResult *))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:356 | 见源码上下文 | True | `malloc(sizeof(ThreadScanResult))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:370 | 见源码上下文 | True | `malloc(results[i]->ext_addr.len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:381 | 见源码上下文 | True | `malloc(results[i]->ext_pan_id.len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:391 | 见源码上下文 | True | `malloc(sizeof(scan_result.network_name))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/network_scan.c:545 | 见源码上下文 | True | `malloc(*outlen)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/scheme_ble.c:81 | 见源码上下文 | True | `malloc(BLE_ADDR_LEN)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/scheme_ble.c:118 | 见源码上下文 | True | `malloc(mfg_data_len)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/scheme_ble.c:131 | new_config | True | `calloc(1, sizeof(protocomm_ble_config_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/scheme_ble.c:240 | 见源码上下文 | True | `realloc(ble_config->nu_lookup, (ble_config->nu_lookup_count + 1) * sizeof(protocomm_ble_name_uuid_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/scheme_console.c:47 | new_config | True | `malloc(sizeof(protocomm_console_config_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__network_provisioning/src/scheme_softap.c:118 | new_config | True | `calloc(1, sizeof(network_prov_softap_config_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/host_test/main/test_main.cpp:40 | __wrap_main | False | `xTaskCreatePinnedToCore(&main_task, "main", ESP_TASK_MAIN_STACK, task_args, ESP_TASK_MAIN_PRIO, NULL, ESP_TASK_MAIN_CORE)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/test_app/main/test_uvc_host.c:70 | test_install_uvc_driver | False | `xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4 * 4096, xTaskGetCurrentTaskHandle(), 10, NULL, 0)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/test_app/main/test_uvc_host.c:360 | TEST_CASE | False | `heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/test_app/main/test_uvc_host.c:362 | TEST_CASE | False | `heap_caps_malloc(buffer_size, MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/test_app/main/test_uvc_host.c:447 | TEST_CASE | False | `xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4 * 4096, xTaskGetCurrentTaskHandle(), 10, NULL, 0)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/test_app/main/test_uvc_host_pm.c:130 | test_install_uvc_driver_pm | False | `xTaskCreatePinnedToCore(usb_lib_task_pm, "usb_lib", 4 * 4096, xTaskGetCurrentTaskHandle(), 10, NULL, 0)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/test_app/main/test_uvc_host_pm.c:145 | test_install_uvc_driver_pm | False | `xQueueCreate(5, sizeof(uvc_host_frame_t *))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/test_app/main/test_uvc_host_pm.c:146 | test_install_uvc_driver_pm | False | `xQueueCreate(3, sizeof(test_uvc_event_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/test_app/main/test_uvc_host_pm.c:439 | TEST_CASE | False | `xTaskCreate(test_fame_handling_task, "frame_handing", 4096, (void *)xTaskGetCurrentTaskHandle(), 4, &frame_handling_task_hdl)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/test_app/main/test_uvc_host_pm.c:502 | TEST_CASE | False | `xTaskCreate(test_fame_handling_task, "frame_handing", 4096, (void *)xTaskGetCurrentTaskHandle(), 4, &frame_handling_task_hdl)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/test_app/main/test_uvc_host_pm.c:703 | TEST_CASE | False | `xTaskCreatePinnedToCore(suspend_task, "suspend_task", 4096, (void *)start_event, 2, &suspend_task_hld, CORE_FOR_SUSPEND)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/test_app/main/test_uvc_host_pm.c:704 | TEST_CASE | False | `xTaskCreatePinnedToCore(disconnect_task, "disconnect_task", 4096, (void *)start_event, 2, &disconnect_task_hdl, CORE_FOR_DISCONNECT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/uvc_frame.c:44 | uvc_frame_allocate | True | `xQueueCreate(nb_of_fb, sizeof(uvc_host_frame_t *))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/uvc_frame.c:49 | uvc_frame_allocate | True | `malloc(sizeof(uvc_host_frame_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/uvc_frame.c:65 | 见源码上下文 | True | `heap_caps_malloc(fb_size, fb_caps)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/uvc_host.c:403 | 见源码上下文 | True | `malloc(num_of_transfers * sizeof(usb_transfer_t *))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/uvc_host.c:477 | uvc_find_and_open_usb_device | True | `calloc(1, sizeof(uvc_stream_t))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/uvc_host.c:655 | 见源码上下文 | True | `heap_caps_calloc(1, sizeof(uvc_host_driver_t), MALLOC_CAP_DEFAULT)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__usb_host_uvc/uvc_host.c:665 | 见源码上下文 | True | `xTaskCreatePinnedToCore( uvc_client_task, "USB-UVC", driver_config->driver_task_stack_size, NULL, driver_config->driver_task_priority, &driver_task_h, driver_config->xCoreID)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__wifi_remote_over_eppp/src/wifi_remote_rpc_client.cpp:146 | init | False | `xTaskCreate(task, "client", 8192, this, 5, nullptr)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__wifi_remote_over_eppp/src/wifi_remote_rpc_server.cpp:84 | init | False | `xQueueCreate(max_items, sizeof(Events))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/espressif__wifi_remote_over_eppp/src/wifi_remote_rpc_server.cpp:133 | init | False | `xTaskCreate(task, "server", 8192, this, 5, nullptr)` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/lvgl__lvgl/src/extra/libs/ffmpeg/lv_ffmpeg.c:678 | 见源码上下文 | True | `calloc(1, sizeof(struct ffmpeg_context_s))` |
| E:/Lummiss_Plant_Robot/src/demo/managed_components/lvgl__lvgl/src/extra/libs/tiny_ttf/stb_truetype_htcw.h:502 | 见源码上下文 | False | `malloc(x)` |