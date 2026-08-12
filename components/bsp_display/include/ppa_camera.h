#ifndef PPA_CAMERA_H
#define PPA_CAMERA_H

#include "lvgl.h"

/* Hook the LVGL image-blit path to use the ESP32-P4 PPA (2D-DMA) for fast
 * 1:1 RGB565 copies, so the camera frame reaches the framebuffer without
 * slow CPU copies. Falls back to LVGL's software draw on any failure. */
void ppa_camera_init(lv_disp_t *disp);

#endif /* PPA_CAMERA_H */
