#ifndef HA_H
#define HA_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Initialise the HA client (must be called once, after WiFi is up). */
esp_err_t ha_init(void);

/* Fetch the raw state string ("on"/"off"/number/...). Returns false on error. */
bool ha_get_state(const char *entity, char *out, size_t out_len);

/* Toggle a light/switch/scene via the REST API. Returns false on error. */
bool ha_toggle(const char *entity);

/* Fetch a sensor value (state string) and its unit (attributes.unit_of_measurement). */
bool ha_get_sensor(const char *entity, char *state, size_t state_len,
                   char *unit, size_t unit_len);

#endif /* HA_H */
