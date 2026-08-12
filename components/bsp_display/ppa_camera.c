#include "ppa_camera.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "draw/sw/lv_draw_sw.h"

#define TAG "PPA"

static ppa_client_handle_t s_ppa = NULL;
static bool s_ppa_ok = false;
static uint32_t s_hits = 0;
static uint32_t s_falls = 0;
static int64_t s_last_log = 0;

static void ppa_log_stats(void)
{
    if (s_ppa_ok && s_hits + s_falls > 0) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now - s_last_log > 10000) {
            s_last_log = now;
            ESP_LOGI(TAG, "ppa blits: %u, sw fallbacks: %u",
                     (unsigned)s_hits, (unsigned)s_falls);
            s_hits = 0;
            s_falls = 0;
        }
    }
}

static void ppa_draw_img_decoded(lv_draw_ctx_t *draw_ctx,
                                 const lv_draw_img_dsc_t *dsc,
                                 const lv_area_t *coords, const uint8_t *map_p,
                                 lv_img_cf_t color_format)
{
    /* fast path: plain 1:1 RGB565 copy, no rotation/zoom/recolor/alpha */
    if (s_ppa_ok && color_format == LV_IMG_CF_TRUE_COLOR &&
        dsc->angle == 0 && dsc->zoom == LV_IMG_ZOOM_NONE &&
        dsc->opa == LV_OPA_COVER && dsc->recolor_opa == LV_OPA_TRANSP) {
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
                s_hits++;
                ppa_log_stats();
                return;
            }
            s_falls++;
            ppa_log_stats();
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
