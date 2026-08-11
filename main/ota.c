#include "ota.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "OTA"

static ota_status_cb_t s_cb = NULL;

static void report(const char *fmt, ...)
{
    if (s_cb) {
        char buf[160];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        s_cb(buf);
    }
}

static void ota_task(void *arg)
{
    const char *url = (const char *)arg;

    report("updating...");

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        report("failed");
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open failed: %s", esp_err_to_name(err));
        report("connect failed");
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len < 0) {
        content_len = 0;
    }
    ESP_LOGI(TAG, "firmware size: %d bytes", content_len);

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        report("no ota slot");
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    esp_ota_handle_t ota = 0;
    err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin failed: %s", esp_err_to_name(err));
        report("flash init failed");
        esp_http_client_cleanup(client);
        vTaskDelete(NULL);
        return;
    }

    char buf[1024];
    int received = 0;
    int last_pct = -1;
    while (1) {
        int r = esp_http_client_read(client, buf, sizeof(buf));
        if (r < 0) {
            ESP_LOGE(TAG, "download error: %d", r);
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        err = esp_ota_write(ota, buf, (size_t)r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota write failed: %s", esp_err_to_name(err));
            break;
        }
        received += r;
        if (content_len > 0) {
            int pct = (received * 100) / content_len;
            if (pct != last_pct) {
                last_pct = pct;
                report("%d%%", pct);
            }
        }
    }

    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        esp_ota_abort(ota);
        report("download failed");
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota end failed: %s", esp_err_to_name(err));
        report("invalid image");
        vTaskDelete(NULL);
        return;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set boot failed: %s", esp_err_to_name(err));
        report("boot set failed");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA ok, rebooting...");
    report("rebooting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    vTaskDelete(NULL);
}

esp_err_t ota_start(const char *url, ota_status_cb_t cb)
{
    if (!url || !*url) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cb = cb;

    char *url_copy = strdup(url);
    if (!url_copy) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(ota_task, "ota", 12288, url_copy, 5, NULL) != pdPASS) {
        free(url_copy);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
