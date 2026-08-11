#include "ui.h"

#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "config.h"

#define TAG "UI"

#define SCR_W 1024
#define SCR_H 600

#define PANEL_L_W 690        /* left camera panel width (left ~2/3) */
#define PANEL_R_X 700        /* right panel start x */
#define PANEL_R_W (SCR_W - PANEL_R_X - 10)

/* palette */
#define C_BG        0x0E1216
#define C_PANEL     0x151B22
#define C_PANEL_R   0x161D25
#define C_TXT       0xE8EAED
#define C_DIM       0x87929C
#define C_ACC       0x2E86DE
#define C_ON        0x1E9E5C
#define C_OFF       0x262E38
#define C_BORDER    0x2A323C

static ui_toggle_fn_t g_toggle_cb = NULL;
static ui_update_fn_t g_update_cb = NULL;

static lv_obj_t *update_btn = NULL;
static lv_obj_t *update_btn_lbl = NULL;

static lv_obj_t *cam_view, *cam_img, *cam_live, *cam_status;
static lv_obj_t *clock_lbl, *date_lbl, *wifi_lbl;
static lv_obj_t *more_overlay, *more_list;

static lv_img_dsc_t cam_dsc;
static uint8_t *s_disp = NULL;       /* viewport-sized RGB565 buffer */
static uint16_t *s_sx = NULL;        /* x sampling table for resize */
static uint16_t s_sw = 0, s_sh = 0;  /* current source size */
static uint16_t s_tw = 0, s_th = 0;  /* current display (target) size */

static lv_obj_t *quick_btns[QUICK_BTNS_COUNT];
static lv_obj_t *extra_btns[EXTRA_BTNS_COUNT];
static lv_obj_t *sensor_val[4]; /* sized by SENSOR_COUNT at runtime */

#define LOCK()  lvgl_port_lock(500)
#define UNLOCK() lvgl_port_unlock()

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, int w, int h,
                          lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_OFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(btn, lv_color_hex(C_TXT), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);

    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    }
    return btn;
}

static void set_btn_state(lv_obj_t *btn, bool on)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(on ? C_ON : C_OFF),
                              LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void quick_evt(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (g_toggle_cb && idx >= 0 && idx < (int)QUICK_BTNS_COUNT) {
        g_toggle_cb(QUICK_BTNS[idx].entity);
    }
}

static void extra_evt(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (g_toggle_cb && idx >= 0 && idx < (int)EXTRA_BTNS_COUNT) {
        g_toggle_cb(EXTRA_BTNS[idx].entity);
    }
}

static void back_evt(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(more_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void more_evt(lv_event_t *e)
{
    (void)e;
    lv_obj_clear_flag(more_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(more_overlay);
}

static void update_evt(lv_event_t *e)
{
    (void)e;
    if (g_update_cb) {
        g_update_cb();
    }
}

static void ui_build_quick(void)
{
    const int cols = 2;
    const int gap = 8;
    const int w = (PANEL_R_W - 24 - (cols - 1) * gap) / cols;
    const int h = 64;
    const int x0 = 12, y0 = 66;

    for (int i = 0; i < (int)QUICK_BTNS_COUNT; i++) {
        int row = i / cols;
        int col = i % cols;
        quick_btns[i] = make_btn(lv_scr_act(), QUICK_BTNS[i].label, w, h,
                                 quick_evt, (void *)(uintptr_t)i);
        lv_obj_set_pos(quick_btns[i], PANEL_R_X + x0 + col * (w + gap),
                       y0 + row * (h + gap));
    }
}

static void ui_build_sensors(lv_obj_t *parent)
{
    const int y0 = 292;
    for (int i = 0; i < (int)SENSOR_COUNT; i++) {
        lv_obj_t *name = lv_label_create(parent);
        lv_label_set_text(name, SENSOR_LIST[i].label);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(C_DIM), 0);
        lv_obj_set_pos(name, PANEL_R_X + 12, y0 + i * 34);

        lv_obj_t *val = lv_label_create(parent);
        lv_label_set_text(val, "--");
        lv_obj_set_style_text_font(val, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(val, lv_color_hex(C_TXT), 0);
        lv_obj_align_to(val, name, LV_ALIGN_OUT_RIGHT_TOP, 40, 0);
        sensor_val[i] = val;
    }
}

static void ui_build_extra_list(void)
{
    more_list = lv_obj_create(more_overlay);
    lv_obj_set_pos(more_list, 12, 66);
    lv_obj_set_size(more_list, SCR_W - 24, SCR_H - 66 - 12);
    lv_obj_set_style_bg_color(more_list, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(more_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(more_list, 8, 0);
    lv_obj_set_style_pad_column(more_list, 0, 0);
    lv_obj_set_style_pad_top(more_list, 0, 0);
    lv_obj_set_style_pad_bottom(more_list, 0, 0);
    lv_obj_set_flex_flow(more_list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < (int)EXTRA_BTNS_COUNT; i++) {
        extra_btns[i] = make_btn(more_list, EXTRA_BTNS[i].label, SCR_W - 24, 56,
                                 extra_evt, (void *)(uintptr_t)i);
    }
}

static void ui_build_overlay(void)
{
    more_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(more_overlay, SCR_W, SCR_H);
    lv_obj_set_pos(more_overlay, 0, 0);
    lv_obj_set_style_bg_color(more_overlay, lv_color_hex(0x0B0E11), 0);
    lv_obj_set_style_border_width(more_overlay, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(more_overlay, 0, 0);
    lv_obj_add_flag(more_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title = lv_label_create(more_overlay);
    lv_label_set_text(title, "ALL LIGHTS");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_30, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TXT), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

    make_btn(more_overlay, "< BACK", 110, 44, back_evt, NULL);

    ui_build_extra_list();
}

void ui_init(void)
{
    if (!LOCK()) return;

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(lv_scr_act(), 0, 0);

    /* ---- left: camera panel ---- */
    lv_obj_t *left = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(left, 0, 0);
    lv_obj_set_size(left, PANEL_L_W, SCR_H);
    lv_obj_set_style_bg_color(left, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_border_width(left, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(left, 0, 0);

    lv_obj_t *cam_title = lv_label_create(left);
    lv_label_set_text(cam_title, "DOOR CAM");
    lv_obj_set_style_text_font(cam_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cam_title, lv_color_hex(C_DIM), 0);
    lv_obj_set_pos(cam_title, 12, 10);

    cam_live = lv_label_create(left);
    lv_label_set_text(cam_live, "LIVE");
    lv_obj_set_style_text_font(cam_live, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(cam_live, lv_color_hex(C_ON), 0);
    lv_obj_align(cam_live, LV_ALIGN_TOP_RIGHT, -12, 10);

    cam_view = lv_obj_create(left);
    lv_obj_set_pos(cam_view, 0, 38);
    lv_obj_set_size(cam_view, PANEL_L_W, SCR_H - 38);
    lv_obj_set_style_bg_color(cam_view, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(cam_view, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(cam_view, 0, 0);

    memset(&cam_dsc, 0, sizeof(cam_dsc));
    cam_dsc.header.always_zero = 0;
    cam_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    cam_dsc.data_size = 0;
    cam_dsc.data = NULL;

    cam_img = lv_img_create(cam_view);
    lv_img_set_src(cam_img, &cam_dsc);
    lv_obj_center(cam_img);

    cam_status = lv_label_create(left);
    lv_label_set_text(cam_status, "camera: offline");
    lv_obj_set_style_text_font(cam_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cam_status, lv_color_hex(0xE05B5B), 0);
    lv_obj_align(cam_status, LV_ALIGN_BOTTOM_LEFT, 12, -8);

    /* ---- right: HA panel ---- */
    lv_obj_t *right = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(right, PANEL_R_X, 0);
    lv_obj_set_size(right, PANEL_R_W, SCR_H);
    lv_obj_set_style_bg_color(right, lv_color_hex(C_PANEL_R), 0);
    lv_obj_set_style_border_width(right, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(right, 0, 0);

    lv_obj_t *ha_title = lv_label_create(right);
    lv_label_set_text(ha_title, "HOME");
    lv_obj_set_style_text_font(ha_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(ha_title, lv_color_hex(C_DIM), 0);
    lv_obj_set_pos(ha_title, 12, 10);

    clock_lbl = lv_label_create(right);
    lv_label_set_text(clock_lbl, "--:--:--");
    lv_obj_set_style_text_font(clock_lbl, &lv_font_montserrat_30, 0);
    lv_obj_set_style_text_color(clock_lbl, lv_color_hex(C_TXT), 0);
    lv_obj_align(clock_lbl, LV_ALIGN_TOP_RIGHT, -12, 8);

    date_lbl = lv_label_create(right);
    lv_label_set_text(date_lbl, "");
    lv_obj_set_style_text_font(date_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(date_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align_to(date_lbl, clock_lbl, LV_ALIGN_OUT_BOTTOM_RIGHT, 0, -2);

    ui_build_quick();
    ui_build_sensors(lv_scr_act());

    update_btn = make_btn(right, "UPDATE", PANEL_R_W - 24, 40,
                          update_evt, NULL);
    lv_obj_align(update_btn, LV_ALIGN_BOTTOM_LEFT, 12, -108);
    update_btn_lbl = lv_obj_get_child(update_btn, 0);

    lv_obj_t *more_btn = make_btn(right, "ALL LIGHTS", PANEL_R_W - 24, 56,
                                  more_evt, NULL);
    lv_obj_align(more_btn, LV_ALIGN_BOTTOM_LEFT, 12, -62);

    wifi_lbl = lv_label_create(right);
    lv_label_set_text(wifi_lbl, "wifi: connecting...");
    lv_obj_set_style_text_font(wifi_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wifi_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(wifi_lbl, LV_ALIGN_BOTTOM_LEFT, 12, -8);

    ui_build_overlay();

    UNLOCK();
}

void ui_set_toggle_callback(ui_toggle_fn_t cb)
{
    g_toggle_cb = cb;
}

void ui_set_update_callback(ui_update_fn_t cb)
{
    g_update_cb = cb;
}

void ui_set_ota_status(const char *text)
{
    if (!LOCK()) return;
    if (update_btn_lbl) {
        lv_label_set_text(update_btn_lbl, text);
    }
    lv_obj_t *btn = lv_obj_get_parent(update_btn_lbl);
    if (btn) {
        lv_obj_set_style_bg_color(btn,
            (strcmp(text, "update failed") == 0) ? lv_color_hex(0x8E2A2A)
                                                 : lv_color_hex(C_OFF),
            LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    UNLOCK();
}

/* aspect-fit the source frame into the camera viewport (no LVGL zoom, which
 * is too slow at 30 fps) */
static void cam_setup_scale(uint16_t w, uint16_t h)
{
    s_sw = w;
    s_sh = h;

    int vw = lv_obj_get_width(cam_view);
    int vh = lv_obj_get_height(cam_view);
    if (vw < 1) vw = 1;
    if (vh < 1) vh = 1;

    double s = (double)vw / (double)w;
    double sy = (double)vh / (double)h;
    if (sy < s) s = sy;
    if (s < 0.05) s = 0.05;

    s_tw = (uint16_t)(w * s);
    s_th = (uint16_t)(h * s);
    if (s_tw < 1) s_tw = 1;
    if (s_th < 1) s_th = 1;

    for (uint16_t x = 0; x < s_tw; x++) {
        s_sx[x] = (uint16_t)(((uint32_t)x * w) / s_tw);
    }
}

static void cam_resize(const uint8_t *src, uint8_t *dst)
{
    if (s_tw == s_sw && s_th == s_sh) {
        memcpy(dst, src, (size_t)s_tw * s_th * 2);
        return;
    }
    uint32_t sstride = (uint32_t)s_sw * 2;
    uint32_t dstride = (uint32_t)s_tw * 2;
    for (uint16_t y = 0; y < s_th; y++) {
        uint16_t sy = (uint16_t)(((uint32_t)y * s_sh) / s_th);
        const uint8_t *srow = src + (uint32_t)sy * sstride;
        uint8_t *drow = dst + (uint32_t)y * dstride;
        uint16_t *spx = (uint16_t *)srow;
        uint16_t *dpx = (uint16_t *)drow;
        for (uint16_t x = 0; x < s_tw; x++) {
            dpx[x] = spx[s_sx[x]];
        }
    }
}

void ui_set_camera_frame(uint16_t w, uint16_t h, const uint8_t *rgb565, uint32_t stride)
{
    (void)stride;
    if (!LOCK()) return;

    if (s_disp && (w != s_sw || h != s_sh)) {
        cam_setup_scale(w, h);
    }
    if (s_disp) {
        cam_resize(rgb565, s_disp);

        cam_dsc.header.w = s_tw;
        cam_dsc.header.h = s_th;
        cam_dsc.data_size = (uint32_t)s_tw * s_th * 2;
        cam_dsc.data = s_disp;

        lv_img_set_src(cam_img, &cam_dsc);
        lv_img_set_zoom(cam_img, 256); /* 1:1 blit */
        lv_obj_center(cam_img);
        lv_obj_invalidate(cam_img);
    }

    UNLOCK();
}

void ui_set_cam_status(bool online)
{
    if (!LOCK()) return;
    lv_label_set_text(cam_status, online ? "camera: live" : "camera: offline");
    lv_obj_set_style_text_color(cam_status, lv_color_hex(online ? C_ON : 0xE05B5B), 0);
    lv_obj_set_style_text_color(cam_live, lv_color_hex(online ? C_ON : 0x555F69), 0);
    UNLOCK();
}

void ui_set_quick_state(int idx, bool on)
{
    if (idx < 0 || idx >= (int)QUICK_BTNS_COUNT) return;
    if (!LOCK()) return;
    set_btn_state(quick_btns[idx], on);
    UNLOCK();
}

void ui_set_extra_state(int idx, bool on)
{
    if (idx < 0 || idx >= (int)EXTRA_BTNS_COUNT) return;
    if (!LOCK()) return;
    set_btn_state(extra_btns[idx], on);
    UNLOCK();
}

void ui_set_sensor_value(int idx, const char *text)
{
    if (idx < 0 || idx >= (int)SENSOR_COUNT) return;
    if (!LOCK()) return;
    lv_label_set_text(sensor_val[idx], text);
    UNLOCK();
}

void ui_set_clock(const char *text)
{
    if (!LOCK()) return;
    lv_label_set_text(clock_lbl, text);
    UNLOCK();
}

void ui_set_date(const char *text)
{
    if (!LOCK()) return;
    lv_label_set_text(date_lbl, text);
    UNLOCK();
}

void ui_set_wifi(const char *text)
{
    if (!LOCK()) return;
    lv_label_set_text(wifi_lbl, text);
    UNLOCK();
}
