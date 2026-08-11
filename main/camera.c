#include "camera.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

/* The Espressif esp_jpeg component exposes its API via "jpeg_decoder.h". */
#include "jpeg_decoder.h"

#include "config.h"
#include "ui.h"
#include "bsp_wifi.h"

#define TAG "CAM"

#define FRAME_MAX_BYTES (512 * 1024)
#define OUTBUF_BYTES    (CAM_MAX_W * CAM_MAX_H * 2)

#define MJPEG_NOFRAME_TIMEOUT_MS  8000   /* switch to snapshot if no MJPEG frames */
#define MJPEG_RETRY_INTERVAL_MS   60000  /* re-test MJPEG while in snapshot mode */
#define SNAPSHOT_INTERVAL_MS      1500

typedef enum {
    CAM_MODE_MJPEG,
    CAM_MODE_SNAPSHOT,
} cam_mode_t;

static uint8_t *s_frame;      /* compressed JPEG accumulator / snapshot buffer */
static size_t s_frame_len = 0;
static bool s_soi_seen = false;
static bool s_prev_ff = false;

static uint8_t *s_out[2];
static int s_front = 0;

/* TJpgDec scratch pool. The default 3100-byte buffer is too small once the
 * JPEG's quantization + huffman tables are also carved out of the pool. */
#define JPEG_WORK_SIZE 16384
static uint8_t *s_jpeg_work;

static cam_mode_t s_mode = CAM_MODE_MJPEG;
static volatile bool s_frame_ok = false;

/* ----------------------------- JPEG decode ------------------------------- */
static void decode_and_show(uint8_t *frame, size_t flen)
{
    esp_jpeg_image_scale_t scale = JPEG_IMAGE_SCALE_0;
    esp_jpeg_image_cfg_t icfg = {
        .indata = frame,
        .indata_size = (uint32_t)flen,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_get_image_info(&icfg, &info) != ESP_OK || info.width == 0) {
        ESP_LOGW(TAG, "get_image_info failed for %u bytes", (unsigned)flen);
        return;
    }
    static uint32_t info_logs = 0;
    if ((info_logs++ % 10) == 0) {
        ESP_LOGI(TAG, "jpeg %ux%u (%u bytes)", info.width, info.height, (unsigned)flen);
    }
    int sw = info.width, sh = info.height;
    while (scale < JPEG_IMAGE_SCALE_1_8 && (sw > CAM_MAX_W || sh > CAM_MAX_H)) {
        scale++;
        sw /= 2;
        sh /= 2;
    }

    int back = 1 - s_front;
    esp_jpeg_image_cfg_t jc = {
        .indata = frame,
        .indata_size = (uint32_t)flen,
        .outbuf = s_out[back],
        .outbuf_size = OUTBUF_BYTES,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = scale,
        .flags = { .swap_color_bytes = CAM_SWAP_BYTES },
        .advanced = { .working_buffer = s_jpeg_work,
                      .working_buffer_size = JPEG_WORK_SIZE },
    };
    esp_jpeg_image_output_t out = {0};
    if (esp_jpeg_decode(&jc, &out) == ESP_OK) {
        s_front = back;
        ui_set_camera_frame(out.width, out.height, s_out[s_front],
                            (uint32_t)out.width * 2);
        s_frame_ok = true;
        static uint32_t frames = 0;
        if ((++frames % 20) == 0) {
            ESP_LOGI(TAG, "%lu frames shown (%dx%d)", (unsigned long)frames,
                     out.width, out.height);
        }
    } else {
        ESP_LOGW(TAG, "jpeg decode failed (%ux%u, %u bytes)", info.width,
                 info.height, (unsigned)flen);
    }
}

/* ----------------------- MJPEG multipart parsing ------------------------- */
static void consume(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (!s_soi_seen) {
            if (s_prev_ff && b == 0xD8) {
                s_frame[0] = 0xFF;
                s_frame[1] = 0xD8;
                s_frame_len = 2;
                s_soi_seen = true;
                s_prev_ff = false;
            } else {
                s_prev_ff = (b == 0xFF);
            }
            continue;
        }

        if (s_frame_len < FRAME_MAX_BYTES - 1) {
            s_frame[s_frame_len++] = b;
            if (b == 0xD9 && s_frame_len >= 2 && s_frame[s_frame_len - 2] == 0xFF) {
                size_t flen = s_frame_len;
                s_frame_len = 0;
                s_soi_seen = false;
                s_prev_ff = false;
                decode_and_show(s_frame, flen);
            }
        } else {
            s_frame_len = 0;
            s_soi_seen = false;
            s_prev_ff = false;
        }
    }
}

static void cam_stream(esp_http_client_handle_t client)
{
    uint8_t *chunk = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk) {
        return;
    }

    while (1) {
        int r = esp_http_client_read(client, (char *)chunk, 8192);
        if (r <= 0) {
            ESP_LOGW(TAG, "stream read ended (%d)", r);
            break;
        }
        consume(chunk, (size_t)r);
    }

    free(chunk);
}

static void build_mjpeg_url(char *buf, size_t len)
{
    snprintf(buf, len, "http://%s:%d/%s/stream.mjpeg?src=%s",
             FRIGATE_HOST, FRIGATE_PORT, GO2RTC_PATH_PREFIX, MJPEG_STREAM_NAME);
}

/* ------------------------------ snapshot -------------------------------- */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
} snap_ctx;

static esp_err_t snap_handler(esp_http_client_event_t *evt)
{
    snap_ctx *c = (snap_ctx *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && c) {
        size_t avail = c->cap - c->len;
        size_t cpy = (evt->data_len < (int)avail) ? (size_t)evt->data_len : avail;
        if (cpy > 0) {
            memcpy(c->buf + c->len, evt->data, cpy);
            c->len += cpy;
        }
    }
    return ESP_OK;
}

static void cam_snapshot(void)
{
    char url[192];
    snprintf(url, sizeof(url), "http://%s:%d/api/%s/latest.jpg",
             FRIGATE_HOST, FRIGATE_PORT, CAMERA_NAME);

    snap_ctx ctx = { .buf = s_frame, .cap = FRAME_MAX_BYTES, .len = 0 };

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 6000,
        .buffer_size = 2048,
        .event_handler = snap_handler,
        .user_data = &ctx,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err == ESP_OK && status == 200 && ctx.len > 0) {
        static uint32_t snaps = 0;
        if ((snaps++ % 10) == 0) {
            ESP_LOGI(TAG, "snapshot fetched: %u bytes", (unsigned)ctx.len);
        }
        decode_and_show(s_frame, ctx.len);
    } else if (err != ESP_OK) {
        ESP_LOGW(TAG, "snapshot request failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "snapshot bad status %d len %u", status, (unsigned)ctx.len);
    }

    esp_http_client_cleanup(client);
}

/* ------------------------------- main loop ------------------------------ */
static void cam_task(void *arg)
{
    (void)arg;

    char mjpeg_url[256];
    build_mjpeg_url(mjpeg_url, sizeof(mjpeg_url));
    ESP_LOGI(TAG, "mjpeg url: %s", mjpeg_url);

    while (bsp_wifi_get_state() != WIFI_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    int64_t last_mjpeg_try = 0;

    while (1) {
        if (s_mode == CAM_MODE_MJPEG) {
            s_frame_ok = false;

            esp_http_client_config_t cfg = {
                .url = mjpeg_url,
                .method = HTTP_METHOD_GET,
                .timeout_ms = 5000,
                .buffer_size = 2048,
                .disable_auto_redirect = true,
            };
            esp_http_client_handle_t client = esp_http_client_init(&cfg);
            if (client) {
                esp_err_t err = esp_http_client_open(client, 0);
                if (err == ESP_OK) {
                    int hlen = esp_http_client_fetch_headers(client);
                    if (hlen >= 0) {
                        ESP_LOGI(TAG, "mjpeg connected (headers %d bytes)", hlen);
                        ui_set_cam_status(true);
                        cam_stream(client);
                    }
                } else {
                    ESP_LOGW(TAG, "mjpeg connect failed: %s", esp_err_to_name(err));
                }
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
            }

            if (!s_frame_ok) {
                ESP_LOGW(TAG, "no MJPEG frames (go2rtc transcode not configured?), using snapshots");
                s_mode = CAM_MODE_SNAPSHOT;
                ui_set_cam_status(false);
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
        } else {
            /* snapshot mode */
            int64_t now = esp_timer_get_time() / 1000;
            if (now - last_mjpeg_try >= MJPEG_RETRY_INTERVAL_MS) {
                last_mjpeg_try = now;
                s_mode = CAM_MODE_MJPEG;   /* give live feed another chance */
                continue;
            }
            cam_snapshot();
            ui_set_cam_status(true);
            vTaskDelay(pdMS_TO_TICKS(SNAPSHOT_INTERVAL_MS));
        }
    }
}

void camera_start(void)
{
    s_frame = heap_caps_malloc(FRAME_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_out[0] = heap_caps_malloc(OUTBUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_out[1] = heap_caps_malloc(OUTBUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_frame || !s_out[0] || !s_out[1]) {
        ESP_LOGE(TAG, "failed to allocate camera buffers");
        return;
    }
    memset(s_out[0], 0, OUTBUF_BYTES);
    memset(s_out[1], 0, OUTBUF_BYTES);
    s_jpeg_work = heap_caps_malloc(JPEG_WORK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_jpeg_work) {
        ESP_LOGE(TAG, "failed to allocate jpeg work buffer");
        return;
    }

    xTaskCreatePinnedToCore(cam_task, "cam", 8192, NULL, 6, NULL, 0);
}
