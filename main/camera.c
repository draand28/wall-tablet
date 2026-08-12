#include "camera.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"
#include "driver/jpeg_types.h"

/* The Espressif esp_jpeg component exposes its API via "jpeg_decoder.h". */
#include "jpeg_decoder.h"

#include "config.h"
#include "ui.h"
#include "bsp_wifi.h"

#define TAG "CAM"

#define FRAME_MAX_BYTES (512 * 1024)
#define OUTBUF_BYTES    (CAM_MAX_W * CAM_MAX_H * 2)
/* hardware decoder outputs rows padded to a multiple of 16 */
#define HW_RAW_BYTES    ((((CAM_MAX_W) + 15) & ~15u) * (((CAM_MAX_H) + 15) & ~15u) * 2)

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

static uint8_t *s_out[2];     /* tightly packed RGB565 display buffers */
static int s_front = 0;

static uint8_t *s_hw_raw;                 /* hardware decoder output */
static jpeg_decoder_handle_t s_hw = NULL; /* hardware JPEG decoder engine */
static bool s_hw_ok = false;

/* TJpgDec scratch pool (software fallback). */
#define JPEG_WORK_SIZE 16384
static uint8_t *s_jpeg_work;

static cam_mode_t s_mode = CAM_MODE_MJPEG;
static volatile bool s_frame_ok = false;

/* frame-rate diagnostics */
static uint32_t s_frames = 0;
static int64_t s_last_fps_log = 0;

static void note_frame(void)
{
    s_frames++;
    int64_t now = esp_timer_get_time() / 1000;
    if (s_last_fps_log == 0) {
        s_last_fps_log = now;
        return;
    }
    if (now - s_last_fps_log >= 5000) {
        ESP_LOGI(TAG, "~%u fps", (unsigned)((s_frames * 1000) / (now - s_last_fps_log)));
        s_frames = 0;
        s_last_fps_log = now;
    }
}

/* Candidate go2rtc MJPEG stream names, probed in order until one yields
 * frames: configured name, camera name, camera name + "_mjpeg". */
static char s_mjpeg_url[3][192];
static int s_mjpeg_url_count = 0;
static int s_active_url = 0;

static void build_mjpeg_urls(void)
{
    char extra[64];
    snprintf(extra, sizeof(extra), "%s_mjpeg", CAMERA_NAME);

    const char *names[3] = { MJPEG_STREAM_NAME, CAMERA_NAME, extra };
    s_mjpeg_url_count = 0;
    for (int i = 0; i < 3; i++) {
        snprintf(s_mjpeg_url[i], sizeof(s_mjpeg_url[0]),
                 "http://%s:%d/%s/stream.mjpeg?src=%s",
                 FRIGATE_HOST, FRIGATE_PORT, GO2RTC_PATH_PREFIX, names[i]);
        s_mjpeg_url_count++;
    }
    s_active_url = 0;
    ESP_LOGI(TAG, "mjpeg candidates: %s | %s | %s", s_mjpeg_url[0],
             s_mjpeg_url[1], s_mjpeg_url[2]);
}

/* ------------------------ software decode (fallback) --------------------- */
static void software_decode(uint8_t *frame, size_t flen)
{
    esp_jpeg_image_scale_t scale = JPEG_IMAGE_SCALE_0;
    esp_jpeg_image_cfg_t icfg = {
        .indata = frame,
        .indata_size = (uint32_t)flen,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
    };
    esp_jpeg_image_output_t info = {0};
    if (esp_jpeg_get_image_info(&icfg, &info) != ESP_OK || info.width == 0) {
        return;
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
    }
}

/* --------------------------- hardware decode ----------------------------- */
static bool hw_decode(uint8_t *frame, size_t flen)
{
    int64_t t0 = esp_timer_get_time();
    if (!s_hw_ok) {
        return false;
    }

    jpeg_decode_picture_info_t info = {0};
    if (jpeg_decoder_get_info(frame, (uint32_t)flen, &info) != ESP_OK ||
        info.width == 0 || info.height == 0) {
        return false;
    }

    uint32_t pw = (info.width + 15) & ~15u;
    uint32_t ph = (info.height + 15) & ~15u;
    if (pw * ph * 2 > HW_RAW_BYTES) {
        return false;
    }

    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR, /* little-endian bytes */
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };

    int back = 1 - s_front;
    uint32_t out_size = 0;
    esp_err_t err = jpeg_decoder_process(s_hw, &dcfg, frame, (uint32_t)flen,
                                         s_hw_raw, HW_RAW_BYTES, &out_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "hw decode failed (%s)", esp_err_to_name(err));
        return false;
    }

    /* strip the 16-px padding so the image is tightly packed for LVGL */
    uint32_t src_stride = pw * 2;
    uint32_t dst_stride = info.width * 2;
    uint8_t *dst = s_out[back];
    for (uint32_t y = 0; y < info.height; y++) {
        memcpy(dst + y * dst_stride, s_hw_raw + y * src_stride, dst_stride);
    }

    s_front = back;
    ui_set_camera_frame(info.width, info.height, s_out[s_front], dst_stride);
    s_frame_ok = true;
    note_frame();

    static int64_t last_log = 0;
    static uint32_t cnt = 0;
    static int64_t sum = 0;
    cnt++;
    sum += esp_timer_get_time() - t0;
    if (esp_timer_get_time() - last_log > 15000000) {
        ESP_LOGI(TAG, "decode cycle avg %lld us (src %ux%u)",
                 (long long)(sum / cnt), (unsigned)info.width, (unsigned)info.height);
        cnt = 0; sum = 0; last_log = esp_timer_get_time();
    }
    return true;
}

static void decode_and_show(uint8_t *frame, size_t flen)
{
    if (!hw_decode(frame, flen)) {
        software_decode(frame, flen);
    }
}

/* ----------------------- MJPEG multipart parsing ------------------------- */
static void maybe_decode(uint8_t *frame, size_t flen);

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
                maybe_decode(s_frame, flen);
            }
        } else {
            s_frame_len = 0;
            s_soi_seen = false;
            s_prev_ff = false;
        }
    }
}

/* ---- decode throttling: decode the newest frame, drop stale ones ----
 * Parsing runs at socket speed so the network buffer never backs up; the
 * decoder only runs on the most recent complete frame, at most every
 * DECODE_INTERVAL_MS. Frames in between are dropped, keeping the picture
 * real-time instead of slowly replaying a backlog. */
#define DECODE_INTERVAL_MS 40

static int64_t s_last_decode_ms = 0;
static uint8_t *s_latest = NULL;
static size_t s_latest_len = 0;

/* always keep the newest complete frame (drop whatever was pending) */
static void maybe_decode(uint8_t *frame, size_t flen)
{
    if (s_latest && flen <= FRAME_MAX_BYTES) {
        memcpy(s_latest, frame, flen);
        s_latest_len = flen;
    }
}

static void maybe_flush_latest(void)
{
    if (s_latest_len) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now - s_last_decode_ms >= DECODE_INTERVAL_MS) {
            s_last_decode_ms = now;
            size_t flen = s_latest_len;
            s_latest_len = 0;
            decode_and_show(s_latest, flen);
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
        maybe_flush_latest();
    }

    free(chunk);
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

    build_mjpeg_urls();

    while (bsp_wifi_get_state() != WIFI_CONNECTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    int64_t last_mjpeg_try = 0;

    while (1) {
        if (s_mode == CAM_MODE_MJPEG) {
            s_frame_ok = false;

            esp_http_client_config_t cfg = {
                .url = s_mjpeg_url[s_active_url],
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
                s_active_url++;
                if (s_active_url >= s_mjpeg_url_count) {
                    s_active_url = 0;
                    ESP_LOGW(TAG, "no MJPEG frames from any stream, using snapshots");
                    s_mode = CAM_MODE_SNAPSHOT;
                    ui_set_cam_status(false);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(1500));
        } else {
            /* snapshot mode */
            int64_t now = esp_timer_get_time() / 1000;
            if (now - last_mjpeg_try >= MJPEG_RETRY_INTERVAL_MS) {
                last_mjpeg_try = now;
                s_active_url = 0;      /* re-probe all candidates */
                s_mode = CAM_MODE_MJPEG;
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
    s_latest = heap_caps_malloc(FRAME_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_out[0] = heap_caps_malloc(OUTBUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_out[1] = heap_caps_malloc(OUTBUF_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_jpeg_work = heap_caps_malloc(JPEG_WORK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_frame || !s_latest || !s_out[0] || !s_out[1] || !s_jpeg_work) {
        ESP_LOGE(TAG, "failed to allocate camera buffers");
        return;
    }

    /* hardware JPEG decode engine */
    jpeg_decode_engine_cfg_t eng = { .intr_priority = 0, .timeout_ms = 200 };
    if (jpeg_new_decoder_engine(&eng, &s_hw) == ESP_OK) {
        jpeg_decode_memory_alloc_cfg_t mcfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        size_t allocated = 0;
        s_hw_raw = jpeg_alloc_decoder_mem(HW_RAW_BYTES, &mcfg, &allocated);
        if (s_hw_raw && allocated >= HW_RAW_BYTES) {
            s_hw_ok = true;
            ESP_LOGI(TAG, "hardware JPEG decoder ready (%u B raw)", (unsigned)allocated);
        } else {
            ESP_LOGW(TAG, "hw decoder mem alloc failed, using software decode");
        }
    } else {
        ESP_LOGW(TAG, "hw decoder engine failed, using software decode");
    }

    xTaskCreatePinnedToCore(cam_task, "cam", 8192, NULL, 6, NULL, 1);
}
