#ifndef OTA_H
#define OTA_H

#include "esp_err.h"

/* Status callback, called from the OTA task with human-readable text. */
typedef void (*ota_status_cb_t)(const char *text);

/* Download <url>, flash it to the next OTA partition and reboot on success.
 * Runs in its own task. */
esp_err_t ota_start(const char *url, ota_status_cb_t cb);

#endif /* OTA_H */
