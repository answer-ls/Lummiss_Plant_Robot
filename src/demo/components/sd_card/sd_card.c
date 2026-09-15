#include "sd_card.h"

#include <string.h>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_pwr_ctrl.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

static const char *TAG = "SD_CARD";

/* TF_VCC 由 P4 片内 LDO 第 4 通道供电，见开发板原理图与厂商 BSP。 */
#define SD_CARD_LDO_CHANNEL     4

/* Slot 0 在 P4 上只走 IOMUX，引脚固定，因此不要在 slot_config 里指定 GPIO。 */
#define SD_CARD_SLOT            SDMMC_HOST_SLOT_0

/* TF 卡的工作电压。sdmmc_card_init 内部也会把 io_voltage 设成这个值
 * （SDMMC_HOST_DEFAULT 的 .io_voltage = 3.3f），这里提前设是为了让它有时间稳定。 */
#define SD_CARD_IO_VOLTAGE_MV   3300

/* LDO 使能后等电源和卡内部上电复位完成，再去发命令。
 * 实测在 LDO 打开后立刻初始化会因为卡还没准备好而在数据阶段超时。 */
#define SD_CARD_POWER_SETTLE_MS 200
/* 初始化失败重试次数与间隔。send_scr 这类数据阶段超时经常重试即可通过。 */
#define SD_CARD_MOUNT_RETRIES   3
#define SD_CARD_RETRY_DELAY_MS  300

static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_pwr_ctrl;
static bool s_mounted;

/* 用指定总线宽度尝试一次挂载。返回底层错误码。 */
static esp_err_t sd_card_try_mount(sdmmc_host_t *host, int width)
{
    /* Slot 0 走 IOMUX，引脚由硬件固定；不要填 clk/cmd/d0..d3。
     *
     * flags 保持 0，与厂商 BSP 的 bsp_sdcard_sdmmc_get_slot() 一致。
     * 不要照抄 esp_hosted 示例的 SDMMC_SLOT_FLAG_INTERNAL_PULLUP：那是给乐鑫
     * 官方 P4 开发板的，本板 TF 座走线不同，开启内部上拉会改变焊盘状态。 */
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = width;
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    slot_config.flags = 0;

    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        /* 当前只会同时打开动画文件和一个备用句柄。FatFS 每个文件都有 4 KB
         * 内部缓存，限制为 2 可给 USB、I2S 和 ESP-Hosted 留出 DMA 内存。 */
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };

    return esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT, host, &slot_config,
                                   &mount_config, &s_card);
}

esp_err_t sd_card_mount(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    /*
     * 关于 esp_hosted 的 `host_sdcard_with_hosted` 示例里的 sdmmc_host_init_dummy：
     * 那个绕法是给 ESP-IDF 6.0+ 用的（示例里用 ESP_IDF_VERSION >= 6.0.0 把关）。
     * 本工程是 5.5.5，sdmmc_host_init() 自带幂等早返回，实测日志确认：
     *
     *     I sdmmc_periph: sdmmc_host_init: SDMMC host already initialized, skipping init flow
     *
     * 所以这里保留默认的 host.init 即可。同理 deinit 也不用替换：
     * sdmmc_host_deinit_slot() 按槽位计数，Slot 1 仍在计数内时会提前返回，
     * 不会把 ESP-Hosted 的链路一起拆掉，因此失败重试是安全的。
     * 反过来，如果照搬示例把 init/deinit 换成空函数，卸载时反而会漏掉引脚释放。
     */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SD_CARD_SLOT;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sd_pwr_ctrl_ldo_config_t ldo_config = {
        .ldo_chan_id = SD_CARD_LDO_CHANNEL,
    };
    esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_config, &s_pwr_ctrl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "片内 LDO 通道 %d 初始化失败：%s",
                 SD_CARD_LDO_CHANNEL, esp_err_to_name(err));
        return err;
    }
    host.pwr_ctrl_handle = s_pwr_ctrl;

    /*
     * 这一行是 0x107 超时的关键。
     *
     * sd_pwr_ctrl_by_on_chip_ldo.c 里创建通道时用的是：
     *     esp_ldo_channel_config_t chan_cfg = { .chan_id = ..., .flags.adjustable = true };
     * voltage_mv 没赋值，保持 0，于是 LDO 按接近下限的电压给 TF_VCC 供电。
     * 实测日志里那句
     *     W ldo: The voltage value 0 is out of the recommended range [500, 2700]
     * 就是它，而且 esp_ldo_channel_adjust_voltage() 只警告不拦截，
     * 0mV 会真的被写进 LDO 的 dref/mul 寄存器。
     *
     * 等到 sdmmc_card_init() 再通过同一个 pwr_ctrl 把电压抬到 3300mV 时，
     * 卡已经被欠压上电、内部复位还没走完，紧接着第一条命令就发出去了。
     * 症状正好是「CMD0/CMD8/ACMD41 这些纯命令行能过，第一条要用 D0 的
     * sdmmc_init_sd_scr 超时」——欠压的卡通常还能应答命令，但驱动不动数据线。
     *
     * 而且 3300 不是个普通电压值：ldo_ll.h 里 LDO_LL_RAIL_VOLTAGE_MV 就是 3300，
     * ldo_ll_voltage_to_dref_mul() 用 `voltage_mv == LDO_LL_RAIL_VOLTAGE_MV`
     * 决定 use_rail_voltage。所以这次调整不只是改电压，是把 LDO 从「DAC 分压」
     * 整个切到「直通电源轨」——一个模式切换，而且就发生在发命令的前一刻。
     * 这也解释了为什么日志里只有一次 voltage 警告：3300 命中 RAIL 分支，不报警。
     *
     * 所以这里抢在通信之前先把电压设对，再让 SD_CARD_POWER_SETTLE_MS 真正
     * 起到「上电稳定」的作用。之后 sdmmc_card_init() 会再设一次同样的值，
     * 属于幂等，不会产生第二遍跳变。
     */
    err = sd_pwr_ctrl_set_io_voltage(s_pwr_ctrl, SD_CARD_IO_VOLTAGE_MV);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "预设 TF_VCC 到 %d mV 失败：%s，仍按默认流程继续",
                 SD_CARD_IO_VOLTAGE_MV, esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "挂载 TF 卡：Slot %d，4 位总线，%d kHz，LDO 通道 %d，TF_VCC %d mV",
             SD_CARD_SLOT, host.max_freq_khz, SD_CARD_LDO_CHANNEL, SD_CARD_IO_VOLTAGE_MV);

    /* 等 TF_VCC 上电稳定。少了这一步，卡可能只够应答命令行、
     * 一到需要 D0 的命令就超时（send_scr returned 0x107）。 */
    vTaskDelay(pdMS_TO_TICKS(SD_CARD_POWER_SETTLE_MS));

    for (int attempt = 1; attempt <= SD_CARD_MOUNT_RETRIES; attempt++) {
        err = sd_card_try_mount(&host, 4);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "第 %d/%d 次挂载失败：%s",
                 attempt, SD_CARD_MOUNT_RETRIES, esp_err_to_name(err));
        if (attempt < SD_CARD_MOUNT_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(SD_CARD_RETRY_DELAY_MS));
        }
    }

    /* 4 位一直失败时退回 1 位：用于区分「D0 有问题」和「D1~D3 有问题」。
     * 1 位只用 CLK/CMD/D0，如果 1 位也不通，问题就集中在 D0 或卡本身。 */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "4 位总线 %d 次均失败，改用 1 位总线做诊断性重试",
                 SD_CARD_MOUNT_RETRIES);
        vTaskDelay(pdMS_TO_TICKS(SD_CARD_RETRY_DELAY_MS));
        err = sd_card_try_mount(&host, 1);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "挂载失败：%s。请确认卡已插到位、为 FAT32、容量不超过 32GB",
                 esp_err_to_name(err));
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl);
        s_pwr_ctrl = NULL;
        return err;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "TF 卡已挂载到 %s", SD_CARD_MOUNT_POINT);
    return ESP_OK;
}

esp_err_t sd_card_unmount(void)
{
    if (!s_mounted) {
        return ESP_OK;
    }

    esp_err_t err = esp_vfs_fat_sdcard_unmount(SD_CARD_MOUNT_POINT, s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "卸载失败：%s", esp_err_to_name(err));
        return err;
    }

    s_card = NULL;
    s_mounted = false;

    if (s_pwr_ctrl != NULL) {
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl);
        s_pwr_ctrl = NULL;
    }

    ESP_LOGI(TAG, "TF 卡已卸载");
    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}
