#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_ldo_regulator.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_wifi.h"

#include "config.h"
#include "ui.h"
#include "camera.h"
#include "ha.h"

#define TAG "MAIN"

#define POLL_STATE_MS   2000
#define POLL_SENSOR_MS  5000

static QueueHandle_t s_cmd_q = NULL;

/* ---------- HA command queue (posted from LVGL event handlers) ---------- */
static void on_toggle(const char *entity)
{
    if (s_cmd_q) {
        xQueueSend(s_cmd_q, &entity, 0);
    }
}

static void refresh_states(void)
{
    char st[24];
    for (int i = 0; i < (int)QUICK_BTNS_COUNT; i++) {
        if (ha_get_state(QUICK_BTNS[i].entity, st, sizeof(st))) {
            ui_set_quick_state(i, !strcmp(st, "on"));
        }
    }
    for (int i = 0; i < (int)EXTRA_BTNS_COUNT; i++) {
        if (ha_get_state(EXTRA_BTNS[i].entity, st, sizeof(st))) {
            ui_set_extra_state(i, !strcmp(st, "on"));
        }
    }
}

static void refresh_sensors(void)
{
    for (int i = 0; i < (int)SENSOR_COUNT; i++) {
        char st[32];
        char unit[16];
        char buf[48];
        if (ha_get_sensor(SENSOR_LIST[i].entity, st, sizeof(st), unit, sizeof(unit))) {
            const char *u = (unit[0]) ? unit : SENSOR_LIST[i].unit;
            snprintf(buf, sizeof(buf), "%s %s", st, u);
            ui_set_sensor_value(i, buf);
        }
    }
}

static void ha_task(void *arg)
{
    (void)arg;
    int64_t last_state = 0, last_sensor = 0;

    while (1) {
        const char *entity = NULL;
        if (s_cmd_q && xQueueReceive(s_cmd_q, &entity, pdMS_TO_TICKS(250)) == pdTRUE) {
            ESP_LOGI(TAG, "toggle %s", entity);
            ha_toggle(entity);
            last_state = 0;
            last_sensor = 0;
        }

        if (bsp_wifi_get_state() == WIFI_CONNECTED) {
            int64_t now = esp_timer_get_time() / 1000;
            if (now - last_state >= POLL_STATE_MS) {
                last_state = now;
                refresh_states();
            }
            if (now - last_sensor >= POLL_SENSOR_MS) {
                last_sensor = now;
                refresh_sensors();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

static void clock_task(void *arg)
{
    (void)arg;
    while (1) {
        char buf[48];
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);

        if (t.tm_year >= 100) { /* clock synced (year >= 2000) */
            strftime(buf, sizeof(buf), "%H:%M:%S", &t);
            ui_set_clock(buf);
            strftime(buf, sizeof(buf), "%a %d %b", &t);
            ui_set_date(buf);
        }

        if (bsp_wifi_get_state() == WIFI_CONNECTED) {
            esp_netif_ip_info_t ip;
            if (esp_netif_get_ip_info(esp_netif_get_default_netif(), &ip) == ESP_OK) {
                snprintf(buf, sizeof(buf), "wifi: " IPSTR, IP2STR(&ip.ip));
            } else {
                snprintf(buf, sizeof(buf), "wifi: connected");
            }
        } else {
            snprintf(buf, sizeof(buf), "wifi: connecting...");
        }
        ui_set_wifi(buf);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void sntp_init_net(void)
{
    setenv("TZ", TZ_STRING, 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

void app_main(void)
{
    /* The P4 LDO3 supplies 2.5 V to the display/camera rail. */
    esp_ldo_channel_handle_t ldo3 = NULL;
    esp_ldo_channel_config_t ldo3_cfg = {
        .chan_id = 3,
        .voltage_mv = 2500,
    };
    esp_ldo_acquire_channel(&ldo3_cfg, &ldo3);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(i2c_init());
    ESP_ERROR_CHECK(touch_init());
    ESP_ERROR_CHECK(display_init());

    bsp_wifi_init();
    bsp_wifi_sta_init();
    bsp_wifi_connect(WIFI_SSID, WIFI_PASSWORD);
    sntp_init_net();

    ha_init();
    ui_set_toggle_callback(on_toggle);
    ui_init();

    set_lcd_blight(100);

    s_cmd_q = xQueueCreate(8, sizeof(const char *));

    xTaskCreatePinnedToCore(ha_task, "ha", 8192, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(clock_task, "clock", 4096, NULL, 4, NULL, 0);
    camera_start();

    ESP_LOGI(TAG, "boot complete");
}
