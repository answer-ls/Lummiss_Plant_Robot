idf_component_register(
    SRCS "person_detect.cpp"
    INCLUDE_DIRS "include"
    REQUIRES coco_detect esp-dl esp_driver_jpeg esp_hw_support esp_timer sd_card video_streamer cloud_mqtt alert_clip freertos heap)

# 推理本身由独立任务执行；组件不修改现有视频、音频和 USB 任务的编译选项。
