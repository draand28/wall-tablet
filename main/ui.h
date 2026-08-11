#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include <stdint.h>

/* Called from other tasks; these take the LVGL lock internally. */
void ui_init(void);

void ui_set_camera_frame(uint16_t w, uint16_t h, const uint8_t *rgb565, uint32_t stride_bytes);
void ui_set_cam_status(bool online);
void ui_set_quick_state(int idx, bool on);
void ui_set_extra_state(int idx, bool on);
void ui_set_sensor_value(int idx, const char *text);
void ui_set_clock(const char *text);
void ui_set_date(const char *text);
void ui_set_wifi(const char *text);

/* Toggle callback: registered by the app layer (posts to HA task). */
typedef void (*ui_toggle_fn_t)(const char *entity);
void ui_set_toggle_callback(ui_toggle_fn_t cb);

/* Over-the-air update button */
typedef void (*ui_update_fn_t)(void);
void ui_set_update_callback(ui_update_fn_t cb);
void ui_set_ota_status(const char *text);

#endif /* UI_H */
