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

#endif /* PPA_CAMERA_H */
