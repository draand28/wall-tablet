#ifndef PPA_CAMERA_H
#define PPA_CAMERA_H

#include "lvgl.h"

/* Hook the LVGL image-blit path to use the ESP32-P4 PPA (2D-DMA) for fast
 * 1:1 RGB565 copies of the camera frame, so it reaches the framebuffer
 * without slow CPU copies. Falls back to LVGL's software draw on any other
 * image or on failure. */
void ppa_camera_init(lv_disp_t *disp);

/* Register the camera's (double-buffered) decoded buffers so the PPA blit
 * only accelerates those. */
void ppa_camera_set_buffers(const uint8_t *a, const uint8_t *b);

/* Direct-to-framebuffer camera path (bypasses the LVGL refresh vsync wait):
 * fetch the DSI frame buffers once. */
void camera_direct_init(void);

/* PPA-copy the camera frame (src_pic_w pixels per row, visible block w x h)
 * into both panel frame buffers at the camera viewport position. Call from
 * the camera decode task at the desired frame rate. */
void camera_direct_show(uint16_t src_pic_w, uint16_t w, uint16_t h,
                        const uint8_t *src);

/* Pause/resume the direct-to-framebuffer blit. Pause while a full-screen LVGL
 * overlay (e.g. "ALL LIGHTS") is open, otherwise the camera would keep
 * overwriting the framebuffer region that LVGL is covering. */
void camera_direct_set_paused(bool paused);

#endif /* PPA_CAMERA_H */
