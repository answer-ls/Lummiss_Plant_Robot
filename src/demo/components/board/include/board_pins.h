#pragma once

#include "driver/gpio.h"

/* 只改这一处即可切换 GPIO：0=当前开发板，1=新 PCB。 */
#define BOARD_USE_NEW_PCB 0

/* 两种板型共用当前已经接好的 ST7789 屏幕引脚。 */
#define BOARD_LCD_RST       GPIO_NUM_1
#define BOARD_LCD_MOSI      GPIO_NUM_2
#define BOARD_LCD_SCLK      GPIO_NUM_3
#define BOARD_LCD_CS        GPIO_NUM_4
#define BOARD_LCD_DC        GPIO_NUM_5
#define BOARD_LCD_TE        GPIO_NUM_6
#define BOARD_LCD_BL        GPIO_NUM_47

#if BOARD_USE_NEW_PCB

/* 新 PCB 音频引脚；此板型不由 ESP32-P4 GPIO 控制功放。 */
#define BOARD_AUDIO_MCLK     GPIO_NUM_28
#define BOARD_AUDIO_BCLK     GPIO_NUM_29
#define BOARD_AUDIO_WS       GPIO_NUM_30
#define BOARD_AUDIO_DOUT     GPIO_NUM_31
#define BOARD_AUDIO_DIN      GPIO_NUM_32
#define BOARD_AUDIO_I2C_SCL  GPIO_NUM_33
#define BOARD_AUDIO_I2C_SDA  GPIO_NUM_34

#define BOARD_HAS_PA_GPIO    0
#define BOARD_HAS_STEPPER    1

/* 新 PCB 的 DRV8833 引脚；BIN2/BIN1 顺序按原理图保留。 */
#define BOARD_STEPPER_FAULT  GPIO_NUM_9
#define BOARD_STEPPER_AIN1   GPIO_NUM_10
#define BOARD_STEPPER_AIN2   GPIO_NUM_11
#define BOARD_STEPPER_BIN2   GPIO_NUM_12
#define BOARD_STEPPER_BIN1   GPIO_NUM_13
#define BOARD_STEPPER_SLEEP  GPIO_NUM_50

#else

/* 当前开发板音频引脚；GPIO9~13 已接入音频，不能用于步进电机。 */
#define BOARD_AUDIO_I2C_SDA  GPIO_NUM_7
#define BOARD_AUDIO_I2C_SCL  GPIO_NUM_8
#define BOARD_AUDIO_DOUT     GPIO_NUM_9
#define BOARD_AUDIO_WS       GPIO_NUM_10
#define BOARD_AUDIO_PA_EN    GPIO_NUM_11
#define BOARD_AUDIO_BCLK     GPIO_NUM_12
#define BOARD_AUDIO_MCLK     GPIO_NUM_13
#define BOARD_AUDIO_DIN      GPIO_NUM_48

#define BOARD_HAS_PA_GPIO    1
#define BOARD_HAS_STEPPER    0

#endif
