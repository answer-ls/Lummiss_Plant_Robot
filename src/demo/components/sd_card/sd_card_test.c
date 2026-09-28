#include "sd_card.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TEST_BYTES 16384
static const char *TAG = "SD_TEST";
static esp_err_t check_write_error(const char *stage, esp_err_t result);

esp_err_t sd_card_official_style_test(void)
{
    // 与 ESP-IDF SDSPI 示例保持相同的文件 API 顺序：挂载→打开写→关闭→重开读。
    // 使用唯一的 8.3 文件名，避免示例中 hello.txt/foo.txt 覆盖已有文件。
    static const char payload[] = "ESP-IDF SDSPI file write check\n";
    esp_err_t result = sd_card_mount();
    if (result != ESP_OK) return result;

    char path[64] = {0};
    struct stat st;
    bool found = false;
    for (unsigned i = 0; i < 1000; ++i) {
        snprintf(path, sizeof(path), SD_CARD_MOUNT_POINT "/SDF%03u.TXT", i);
        errno = 0;
        if (stat(path, &st) != 0 && errno == ENOENT) {
            found = true;
            break;
        }
    }
    if (!found) {
        ESP_LOGE(TAG, "OFFICIAL_STYLE no free test filename");
        result = ESP_ERR_NOT_FOUND;
        goto done;
    }

    ESP_LOGI(TAG, "OFFICIAL_STYLE WRITE_OPEN path=%s bytes=%u", path,
             (unsigned)(sizeof(payload) - 1));
    FILE *file = fopen(path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "OFFICIAL_STYLE WRITE_OPEN failed errno=%d", errno);
        result = ESP_FAIL;
        goto done;
    }
    size_t written = fwrite(payload, 1, sizeof(payload) - 1, file);
    int close_result = fclose(file);
    ESP_LOGI(TAG, "OFFICIAL_STYLE WRITE_CLOSE written=%u close=%d low_level=%s",
             (unsigned)written, close_result, esp_err_to_name(sd_card_get_write_error()));
    if (written != sizeof(payload) - 1 || close_result != 0) result = ESP_FAIL;
    result = check_write_error("OFFICIAL_STYLE_WRITE_CLOSE", result);
    if (result != ESP_OK) goto done;

    file = fopen(path, "rb");
    if (!file) {
        ESP_LOGE(TAG, "OFFICIAL_STYLE READ_OPEN failed errno=%d", errno);
        result = ESP_FAIL;
        goto done;
    }
    char actual[sizeof(payload)] = {0};
    size_t read_len = fread(actual, 1, sizeof(payload) - 1, file);
    int trailing = fgetc(file);
    bool read_error = ferror(file);
    close_result = fclose(file);
    bool match = read_len == sizeof(payload) - 1 && trailing == EOF &&
                 !read_error && close_result == 0 &&
                 memcmp(actual, payload, sizeof(payload) - 1) == 0;
    ESP_LOGI(TAG, "OFFICIAL_STYLE READBACK read=%u match=%d close=%d",
             (unsigned)read_len, match, close_result);
    if (!match) result = ESP_ERR_INVALID_RESPONSE;

done:
    // 成功后保留测试文件供电脑核对；失败时也不删除或格式化卡上数据。
    esp_err_t cleanup = sd_card_unmount();
    if (result == ESP_OK) result = cleanup;
    ESP_LOGI(TAG, "OFFICIAL_STYLE RESULT=%s path=%s", esp_err_to_name(result), path);
    return result;
}

static esp_err_t check_write_error(const char *stage, esp_err_t result)
{
    esp_err_t low_level = sd_card_get_write_error();
    if (low_level != ESP_OK) {
        ESP_LOGE(TAG, "%s: low-level write failed: %s (even if filesystem API succeeded)",
                 stage, esp_err_to_name(low_level));
        return low_level;
    }
    return result;
}

static esp_err_t verify_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_FAIL;
    unsigned char data[256];
    bool ok = true;
    for (unsigned offset = 0; offset < TEST_BYTES && ok; offset += sizeof(data)) {
        if (fread(data, 1, sizeof(data), f) != sizeof(data)) { ok = false; break; }
        for (unsigned i = 0; i < sizeof(data); ++i) {
            if (data[i] != (unsigned char)(((offset + i) * 37U) ^ ((offset + i) >> 8))) {
                ESP_LOGE(TAG, "Mismatch at byte %u", offset + i);
                ok = false;
                break;
            }
        }
    }
    if (ok && (fgetc(f) != EOF || ferror(f))) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t sd_card_self_test(void)
{
    esp_err_t err = sd_card_mount();
    if (err != ESP_OK) return err;
    char path[64];
    int fd = -1;
    /* Exclusive creation preserves all pre-existing files, including old tests. */
    for (unsigned i = 0; i < 1000; ++i) {
        snprintf(path, sizeof(path), SD_CARD_MOUNT_POINT "/SDT%03u.BIN", i);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 || errno != EEXIST) break;
        vTaskDelay(1);
    }
    if (fd < 0) {
        ESP_LOGE(TAG, "Create test file failed, errno=%d", errno);
        sd_card_unmount();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Write %d bytes: %s", TEST_BYTES, path);
    unsigned char data[256];
    for (unsigned offset = 0; offset < TEST_BYTES; offset += sizeof(data)) {
        for (unsigned i = 0; i < sizeof(data); ++i)
            data[i] = (unsigned char)(((offset + i) * 37U) ^ ((offset + i) >> 8));
        ssize_t written = write(fd, data, sizeof(data));
        if (written != sizeof(data) || sd_card_get_write_error() != ESP_OK) {
            ESP_LOGE(TAG, "WRITE failed: offset=%u returned=%d errno=%d",
                     offset, (int)written, errno);
            err = ESP_FAIL;
            break;
        }
        vTaskDelay(1);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "FSYNC begin");
        if (fsync(fd) != 0) {
            ESP_LOGE(TAG, "FSYNC failed: errno=%d", errno);
            err = ESP_FAIL;
        }
        err = check_write_error("FSYNC", err);
    }
    vTaskDelay(1);
    /* close is required even after failure; FatFs may still attempt a flush. */
    if (close(fd) != 0) {
        ESP_LOGE(TAG, "CLOSE failed: errno=%d", errno);
        err = ESP_FAIL;
    }
    err = check_write_error("CLOSE", err);
    vTaskDelay(1);
    if (err == ESP_OK) err = verify_file(path);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Initial readback PASS; unmount/remount verification");
        err = sd_card_unmount();
        if (err == ESP_OK) err = sd_card_mount();
        if (err == ESP_OK) err = verify_file(path);
    }
    if (sd_card_is_mounted()) {
        if (err == ESP_OK) {
            if (unlink(path) != 0) err = ESP_FAIL;
            err = check_write_error("CLEANUP", err);
        } else {
            ESP_LOGW(TAG, "Skip deletion after IO failure; test file may remain: %s", path);
        }
        esp_err_t cleanup = sd_card_unmount();
        if (err == ESP_OK) err = cleanup;
    } else {
        ESP_LOGW(TAG, "Card unavailable; test file may remain: %s", path);
    }
    ESP_LOGI(TAG, "write/read/remount/read/cleanup: %s", err == ESP_OK ? "PASS" : "FAIL");
    return err;
}
