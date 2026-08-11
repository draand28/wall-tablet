#include "log_server.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TAG "LOGSRV"
#define LOG_BUF_SIZE 65536

static char s_buf[LOG_BUF_SIZE];
static size_t s_len = 0;
static SemaphoreHandle_t s_mutex = NULL;

static void ring_write(const char *data, size_t len)
{
    if (s_len + len > LOG_BUF_SIZE) {
        size_t drop = s_len + len - LOG_BUF_SIZE;
        memmove(s_buf, s_buf + drop, s_len - drop);
        s_len -= drop;
    }
    memcpy(s_buf + s_len, data, len);
    s_len += len;
}

int log_server_vprintf(const char *fmt, va_list args)
{
    char line[512];
    int len = vsnprintf(line, sizeof(line), fmt, args);
    if (len <= 0) {
        return len;
    }

    /* keep UART output on COM3 */
    fwrite(line, 1, (size_t)len, stdout);

    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    ring_write(line, (size_t)len);
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }

    return len;
}

static esp_err_t handle_root(httpd_req_t *req)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    time_t now = tv.tv_sec;
    struct tm t;
    localtime_r(&now, &t);
    char ts[48];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &t);

    char html[768];
    int n = snprintf(html, sizeof(html),
        "<html><head><meta charset=\"utf-8\"><meta http-equiv=\"refresh\" content=\"5\">"
        "<title>Wall Tablet</title></head><body style=\"font-family:monospace;background:#111;color:#eee\">"
        "<h2>Wall Tablet</h2>"
        "<p>time: %s</p>"
        "<p><a href=\"/log\">/log &rarr; recent device log</a></p>"
        "<p><a href=\"/log\" onclick=\"fetch('/log').then(r=>r.text()).then(t=>document.getElementById('l').textContent=t);return false;\">"
        "reload log</a></p>"
        "<pre id=\"l\"></pre>"
        "</body></html>",
        ts);
    if (n < 0) {
        n = 0;
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, html, (size_t)n);
}

static esp_err_t handle_log(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
    size_t start = (s_len > 32768) ? s_len - 32768 : 0;
    size_t len = s_len - start;
    esp_err_t ret = httpd_resp_send_chunk(req, s_buf + start, len);
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
    if (ret != ESP_OK) {
        return ret;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

void log_server_start(void)
{
    s_mutex = xSemaphoreCreateMutex();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 8;

    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "http server failed to start");
        return;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = handle_root, .user_ctx = NULL };
    httpd_uri_t log  = { .uri = "/log", .method = HTTP_GET, .handler = handle_log, .user_ctx = NULL };
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &log);

    ESP_LOGI(TAG, "status/log server on :80");
}
