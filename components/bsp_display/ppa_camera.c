#include "ppa_camera.h"
#include "bsp_display.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "draw/sw/lv_draw_sw.h"

#define TAG "PPA"

#define FB_W 1024
#define FB_H 600
#define CAM_VIEW_W 690
#define CAM_VIEW_H (FB_H - 38)

static ppa_client_handle_t s_ppa = NULL;
static bool s_ppa_ok = false;
static const uint8_t *s_cam_bufs[2] = { NULL, NULL };
static void *s_fb[2] = { NULL, NULL };

void ppa_camera_set_buffers(const uint8_t *a, const uint8_t *b)
{
    s_cam_bufs[0] = a;
    s_cam_bufs[1] = b;
}

void camera_direct_init(void)
{
    bsp_get_display_framebuffers(&s_fb[0], &s_fb[1]);
}

void camera_direct_show(uint16_t src_pic_w, uint16_t w, uint16_t h,
                        const uint8_t *src)
{
    if (!s_fb[0] || !s_fb[1]) {
        return;
    }
    /* centre-crop into the 690-wide viewport */
    uint16_t cw = (w > CAM_VIEW_W) ? CAM_VIEW_W : w;
    uint16_t src_x = (w - cw) / 2;
    uint16_t dst_x = (CAM_VIEW_W - cw) / 2;
    int y = (CAM_VIEW_H - h) / 2;
    if (y < 0) {
        y = 0;
    }

    ppa_srm_oper_config_t op = {0};
    op.in.buffer = src;
    op.in.pic_w = src_pic_w;
    op.in.pic_h = h;
    op.in.block_offset_x = src_x;
    op.in.block_offset_y = 0;
    op.in.block_w = cw;
    op.in.block_h = h;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    op.out.buffer_size = (uint32_t)FB_W * FB_H * 2;
    op.out.pic_w = FB_W;
    op.out.pic_h = FB_H;
    op.out.block_offset_x = dst_x;
    op.out.block_offset_y = y;
    op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x = 1.0f;
    op.scale_y = 1.0f;
    op.mirror_x = false;
    op.mirror_y = false;
    op.mode = PPA_TRANS_MODE_BLOCKING;

    op.out.buffer = s_fb[0];
    ppa_do_scale_rotate_mirror(s_ppa, &op);
    op.out.buffer = s_fb[1];
    ppa_do_scale_rotate_mirror(s_ppa, &op);
}

static void ppa_draw_img_decoded(lv_draw_ctx_t *draw_ctx,
                                 const lv_draw_img_dsc_t *dsc,
                                 const lv_area_t *coords, const uint8_t *map_p,
                                 lv_img_cf_t color_format)
{
    /* only accelerate the camera frame buffer blits */
    if (!s_ppa_ok || (map_p != s_cam_bufs[0] && map_p != s_cam_bufs[1])) {
        lv_draw_sw_img_decoded(draw_ctx, dsc, coords, map_p, color_format);
        return;
    }

    if (color_format == LV_IMG_CF_TRUE_COLOR && dsc->angle == 0 &&
        dsc->zoom == LV_IMG_ZOOM_NONE && dsc->opa == LV_OPA_COVER &&
        dsc->recolor_opa == LV_OPA_TRANSP) {
        lv_area_t vis;
        if (_lv_area_intersect(&vis, draw_ctx->clip_area, coords)) {
            lv_coord_t img_w = lv_area_get_width(coords);
            lv_coord_t img_h = lv_area_get_height(coords);
            lv_coord_t fb_w = lv_area_get_width(draw_ctx->buf_area);
            lv_coord_t fb_h = lv_area_get_height(draw_ctx->buf_area);
            lv_coord_t vis_w = lv_area_get_width(&vis);
            lv_coord_t vis_h = lv_area_get_height(&vis);

            ppa_srm_oper_config_t op = {0};
            op.in.buffer = map_p;
            op.in.pic_w = img_w;
            op.in.pic_h = img_h;
            op.in.block_offset_x = (uint32_t)(vis.x1 - coords->x1);
            op.in.block_offset_y = (uint32_t)(vis.y1 - coords->y1);
            op.in.block_w = vis_w;
            op.in.block_h = vis_h;
            op.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

            op.out.buffer = draw_ctx->buf;
            op.out.buffer_size = (uint32_t)fb_w * fb_h * sizeof(lv_color_t);
            op.out.pic_w = fb_w;
            op.out.pic_h = fb_h;
            op.out.block_offset_x = (uint32_t)(vis.x1 - draw_ctx->buf_area->x1);
            op.out.block_offset_y = (uint32_t)(vis.y1 - draw_ctx->buf_area->y1);
            op.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

            op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
            op.scale_x = 1.0f;
            op.scale_y = 1.0f;
            op.mirror_x = false;
            op.mirror_y = false;
            op.mode = PPA_TRANS_MODE_BLOCKING;

            if (ppa_do_scale_rotate_mirror(s_ppa, &op) == ESP_OK) {
                return;
            }
            ESP_LOGW(TAG, "ppa srm failed, using sw draw");
        }
    }
    lv_draw_sw_img_decoded(draw_ctx, dsc, coords, map_p, color_format);
}

void ppa_camera_init(lv_disp_t *disp)
{
    ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    if (ppa_register_client(&cfg, &s_ppa) != ESP_OK) {
        ESP_LOGW(TAG, "ppa client init failed");
        return;
    }
    s_ppa_ok = true;

    if (disp && disp->driver && disp->driver->draw_ctx) {
        disp->driver->draw_ctx->draw_img_decoded = ppa_draw_img_decoded;
    }
    ESP_LOGI(TAG, "PPA 2D blit enabled");
}
