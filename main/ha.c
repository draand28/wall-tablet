#include "ha.h"

#include <stdio.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "config.h"

#define TAG "HA"

#define HA_RESP_MAX 4096

static SemaphoreHandle_t s_mutex = NULL;

static void lock(void)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t ha_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    return (s_mutex != NULL) ? ESP_OK : ESP_FAIL;
}

static void build_url(char *buf, size_t len, const char *path)
{
    snprintf(buf, len, "http://%s:%d%s", HA_HOST, HA_PORT, path);
}

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} resp_ctx;

static esp_err_t resp_handler(esp_http_client_event_t *evt)
{
    resp_ctx *ctx = (resp_ctx *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && ctx->buf && ctx->cap > 1) {
        size_t avail = ctx->cap - ctx->len - 1;
        size_t cpy = (evt->data_len < (int)avail) ? (size_t)evt->data_len : avail;
        memcpy(ctx->buf + ctx->len, evt->data, cpy);
        ctx->len += cpy;
        ctx->buf[ctx->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t http_request(const char *path, const char *method,
                              const char *body, char *resp, size_t resp_len)
{
    char url[256];
    build_url(url, sizeof(url), path);

    resp_ctx ctx = { .buf = resp, .len = 0, .cap = resp_len };

    esp_http_client_config_t cfg = {
        .url = url,
        .method = (method && !strcmp(method, "POST")) ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .timeout_ms = 6000,
        .buffer_size = 2048,
        .event_handler = resp_handler,
        .user_data = (resp && resp_len) ? &ctx : NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }
    if (resp && resp_len) {
        resp[0] = '\0';
    }

    esp_http_client_set_header(client, "Authorization", "Bearer " HA_TOKEN);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    if (body) {
        esp_http_client_set_post_field(client, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    if (err == ESP_OK && status >= 400) {
        ESP_LOGW(TAG, "HTTP %d for %s: %s", status, path, resp ? resp : "");
        err = ESP_FAIL;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "request failed (%s): %s", method ? method : "GET",
                 esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return err;
}

/* POST a JSON body, e.g. {"entity_id":"light.x"} */
static esp_err_t http_post_json(const char *path, const char *json_body)
{
    return http_request(path, "POST", json_body, NULL, 0);
}

/* Strip a cJSON string value, handling null/errors. */
static bool json_get_string(const char *json, const char *key,
                            char *out, size_t out_len)
{
    if (!json || !*json) {
        return false;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    bool ok = false;
    cJSON *node = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(node) && node->valuestring) {
        snprintf(out, out_len, "%s", node->valuestring);
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}

bool ha_get_state(const char *entity, char *out, size_t out_len)
{
    char path[192];
    char resp[HA_RESP_MAX];

    snprintf(path, sizeof(path), "/api/states/%s", entity);

    lock();
    esp_err_t err = http_request(path, "GET", NULL, resp, sizeof(resp));
    unlock();

    if (err != ESP_OK) {
        return false;
    }
    return json_get_string(resp, "state", out, out_len);
}

bool ha_get_sensor(const char *entity, char *state, size_t state_len,
                   char *unit, size_t unit_len)
{
    char path[192];
    char resp[HA_RESP_MAX];

    snprintf(path, sizeof(path), "/api/states/%s", entity);

    lock();
    esp_err_t err = http_request(path, "GET", NULL, resp, sizeof(resp));
    unlock();

    if (err != ESP_OK) {
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    if (!root) {
        return false;
    }
    bool ok = false;
    cJSON *s = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (cJSON_IsString(s) && s->valuestring) {
        snprintf(state, state_len, "%s", s->valuestring);
        ok = true;
    }
    if (unit && unit_len) {
        unit[0] = '\0';
        cJSON *attrs = cJSON_GetObjectItemCaseSensitive(root, "attributes");
        if (cJSON_IsObject(attrs)) {
            cJSON *u = cJSON_GetObjectItemCaseSensitive(attrs, "unit_of_measurement");
            if (cJSON_IsString(u) && u->valuestring) {
                snprintf(unit, unit_len, "%s", u->valuestring);
            }
        }
    }
    cJSON_Delete(root);
    return ok;
}

bool ha_toggle(const char *entity)
{
    char domain[32];
    const char *dot = strchr(entity, '.');
    if (!dot || dot == entity) {
        return false;
    }
    size_t dlen = (size_t)(dot - entity);
    if (dlen >= sizeof(domain)) {
        dlen = sizeof(domain) - 1;
    }
    memcpy(domain, entity, dlen);
    domain[dlen] = '\0';

    const char *service = "toggle";
    if (!strcmp(domain, "scene")) {
        service = "turn_on";
    } else if (!strcmp(domain, "input_boolean")) {
        service = "toggle";
    }

    char path[96];
    snprintf(path, sizeof(path), "/api/services/%s/%s", domain, service);

    char body[160];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", entity);

    lock();
    esp_err_t err = http_post_json(path, body);
    unlock();

    return (err == ESP_OK);
}
