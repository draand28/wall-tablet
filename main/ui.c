#include "ui.h"

#include <string.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"

#include "config.h"
#include "ppa_camera.h"

#define TAG "UI"

#define SCR_W 1024
#define SCR_H 600

#define PANEL_L_W 690        /* left camera panel width (left ~2/3) */
#define PANEL_R_X 700        /* right panel start x */
#define PANEL_R_W (SCR_W - PANEL_R_X - 10)

#define CAM_VIEW_W (PANEL_L_W)
#define CAM_VIEW_H (SCR_H - 38)

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

static lv_obj_t *cam_view, *cam_live, *cam_status;
static lv_obj_t *cam_fps_lbl;
static lv_obj_t *clock_lbl, *date_lbl, *wifi_lbl;
static lv_obj_t *more_overlay, *more_list;

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
    camera_direct_set_paused(false);
    lv_obj_add_flag(more_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void more_evt(lv_event_t *e)
{
    (void)e;
    camera_direct_set_paused(true);
    lv_obj_clear_flag(more_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(more_overlay);
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

#define EXTRA_COLS 4
#define EXTRA_BOX_H 110
#define EXTRA_GAP 12

static void ui_build_extra_list(void)
{
    static lv_coord_t col_dsc[EXTRA_COLS + 1];
    for (int c = 0; c < EXTRA_COLS; c++) {
        col_dsc[c] = LV_GRID_FR(1);
    }
    col_dsc[EXTRA_COLS] = LV_GRID_TEMPLATE_LAST;

    static lv_coord_t row_dsc[2 + 1];
    row_dsc[0] = EXTRA_BOX_H;
    row_dsc[1] = EXTRA_BOX_H;
    row_dsc[2] = LV_GRID_TEMPLATE_LAST;

    more_list = lv_obj_create(more_overlay);
    lv_obj_set_pos(more_list, 12, 66);
    lv_obj_set_size(more_list, SCR_W - 24, SCR_H - 66 - 12);
    lv_obj_set_style_bg_color(more_list, lv_color_hex(C_BG), 0);
    lv_obj_set_style_border_width(more_list, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(more_list, 0, 0);
    lv_obj_set_style_pad_row(more_list, EXTRA_GAP, 0);
    lv_obj_set_style_pad_column(more_list, EXTRA_GAP, 0);
    lv_obj_set_grid_dsc_array(more_list, col_dsc, row_dsc);

    for (int i = 0; i < (int)EXTRA_BTNS_COUNT; i++) {
        extra_btns[i] = make_btn(more_list, EXTRA_BTNS[i].label, 0, 0,
                                 extra_evt, (void *)(uintptr_t)i);
        lv_obj_set_grid_cell(extra_btns[i], LV_GRID_ALIGN_STRETCH,
                             i % EXTRA_COLS, 1, LV_GRID_ALIGN_STRETCH,
                             i / EXTRA_COLS, 1);
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

    cam_status = lv_label_create(left);
    lv_label_set_text(cam_status, "camera: offline");
    lv_obj_set_style_text_font(cam_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cam_status, lv_color_hex(0xE05B5B), 0);
    lv_obj_align(cam_status, LV_ALIGN_BOTTOM_LEFT, 12, -8);

    cam_fps_lbl = lv_label_create(left);
    lv_label_set_text(cam_fps_lbl, "");
    lv_obj_set_style_text_font(cam_fps_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cam_fps_lbl, lv_color_hex(C_DIM), 0);
    lv_obj_align(cam_fps_lbl, LV_ALIGN_BOTTOM_RIGHT, -12, -8);

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
    lv_obj_align_to(date_lbl, clock_lbl, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);

    ui_build_quick();
    ui_build_sensors(lv_scr_act());

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
    (void)text; /* no on-screen button; OTA status is visible in /log */
}

void ui_set_cam_status(bool online)
{
    if (!LOCK()) return;
    lv_label_set_text(cam_status, online ? "camera: live" : "camera: offline");
    lv_obj_set_style_text_color(cam_status, lv_color_hex(online ? C_ON : 0xE05B5B), 0);
    lv_obj_set_style_text_color(cam_live, lv_color_hex(online ? C_ON : 0x555F69), 0);
    UNLOCK();
}

void ui_set_cam_fps(const char *text)
{
    if (!LOCK()) return;
    lv_label_set_text(cam_fps_lbl, text);
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
