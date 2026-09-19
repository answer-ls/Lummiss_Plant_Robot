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

/* ========================================================================
 * 人机交互外设（机械按键 / TTP223 触摸 / WS2812 氛围灯）
 *
 * 重要前提：这三组外设目前**只存在于将来要打的新 PCB 上**，当前开发板上
 * 并没有这三处接线。所以按板型分开定义 —— 改开发板那一份不会碰到新 PCB。
 * 驱动组件一律引用这里的宏，不要在 .c 里出现 GPIO 数字。
 *
 * 开发板上实测已知的两件事（别再当成"空闲脚"用）：
 *   1. GPIO22 在这块板上接的是 RST 复位键：按下会直接复位芯片，所以它在
 *      开发板上**不能**当普通按键输入来验证（驱动本身只做输入+上拉，安全）。
 *   2. GPIO51/52 上电读到高电平（档位 9 日志里的"幽灵长按"就是它）。这两个脚
 *      在开发板上究竟接到了什么，需要查原理图确认后再决定能不能借用。
 *
 * GPIO20 那条"悬案"已结案：README 与 PROJECT_HANDOFF 的旧版把屏幕 SCLK 写成
 * GPIO20，与 BOARD_LCD_SCLK=GPIO3 矛盾。实际情况是屏幕为**外接** ST7789，
 * 接在 1~6/47 上，与 GPIO20 无关（两份文档已按此更正）。所以 WS2812_A 用
 * GPIO20，在开发板和新 PCB 上都不冲突。
 * ======================================================================== */

#if BOARD_USE_NEW_PCB

/* 新 PCB（2026-09 定版）的人机交互引脚。 */
#define BOARD_WS2812_A_GPIO      GPIO_NUM_20
#define BOARD_WS2812_B_GPIO      GPIO_NUM_21
#define BOARD_KEY_GPIO           GPIO_NUM_22
#define BOARD_TOUCH_1_GPIO       GPIO_NUM_52
#define BOARD_TOUCH_2_GPIO       GPIO_NUM_51

#else

/* 当前开发板：这三组外设还没有接线。下面这几个值只是**占位**，让驱动能
 * 编译、让档位 9 的自检流程能跑通；它不能作为"开发板上这三个外设可用"的
 * 证据。要在开发板上真正验证，把这里换成确认空闲的脚，再用杜邦线短接测
 * （KEY 短到 GND、TOUCH 短到 3V3），整条"电平→去抖→事件→回调"都能走通。 */
#define BOARD_WS2812_A_GPIO      GPIO_NUM_20
#define BOARD_WS2812_B_GPIO      GPIO_NUM_21
#define BOARD_KEY_GPIO           GPIO_NUM_22
#define BOARD_TOUCH_1_GPIO       GPIO_NUM_52
#define BOARD_TOUCH_2_GPIO       GPIO_NUM_51

#endif

/* 灯珠数量必须与实物一致：写多了只是多发一段无效数据（无害），写少了末尾
 * 灯珠不亮。改这里即可，不用动驱动代码。两种板型共用。 */
#define BOARD_WS2812_A_LED_COUNT 20
#define BOARD_WS2812_B_LED_COUNT 20

/* TTP223 电容触摸按键：模块已在硬件侧完成电容检测并输出数字电平，
 * ESP32-P4 只把它当普通 GPIO 输入读，不使用 P4 内部触摸外设。
 * TOUCH_1 对应 TTP223_OUT1，TOUCH_2 对应 TTP223_OUT2；与"左/右"的对应关系
 * 尚未确定，所以驱动层只叫 TOUCH_1 / TOUCH_2。 */

/* TTP223 输出极性：0 = 触摸时输出高电平（模块出厂默认，背面 A 焊盘断开）；
 *                 1 = 触摸时输出低电平（A 焊盘被短接）。
 * 改这一处即可，touch_key.c 只认这个宏。两种板型共用。 */
#define BOARD_TOUCH_ACTIVE_LOW   0
