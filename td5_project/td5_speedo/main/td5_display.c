/*
 * TD5 Display - Speedometer GUI using LVGL 9.x
 * Layout for landscape 800×480 IPS panel.
 *
 * Fonts: Arial Bold (all-caps text), DSEG7 Classic Bold (numbers)
 * Background: pure black
 */
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "lvgl.h"
#include "td5_display.h"
#include "td5_usb.h"

/* Custom fonts (generated with lv_font_conv) */
LV_FONT_DECLARE(font_arial_bold_16);
LV_FONT_DECLARE(font_dseg7_bold_270);
LV_FONT_DECLARE(font_montserrat_bold_72);

static const char *TAG = "td5_disp";

static lv_obj_t *s_status_label;
static lv_obj_t *s_speed_ghost;   /* dim "888" background */
static lv_obj_t *s_speed_label;
static lv_obj_t *s_unit_label;

/* Persistent buffer for speed text — avoids lv_realloc fragmentation.
 * LVGL's lv_label_set_text_static() stores only the pointer, so this
 * buffer must survive between calls. */
static char s_speed_buf[8] = "0";

#define COL_BG          lv_color_hex(0x000000)
#define COL_ARC_BG      lv_color_hex(0x1C2333)
#define COL_ARC_GREEN   lv_color_hex(0x00E676)
#define COL_ARC_YELLOW  lv_color_hex(0xFFD600)
#define COL_ARC_RED     lv_color_hex(0xFF1744)
#define COL_TEXT_PRI    lv_color_hex(0xE6EDF3)
#define COL_TEXT_SEC    lv_color_hex(0x8B949E)
#define COL_TEXT_DIM    lv_color_hex(0x484F58)
#define COL_ACCENT      lv_color_hex(0x58A6FF)



void td5_display_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Status bar ─────────────────────────────────────── */
    s_status_label = lv_label_create(scr);
    lv_label_set_text(s_status_label, "CONNECTING TO TD5 ECU...");
    lv_obj_set_style_text_color(s_status_label, COL_ACCENT, 0);
    lv_obj_set_style_text_font(s_status_label, &font_arial_bold_16, 0);
    lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 8);

    /* ── Speed number (centred) ──────────────────────────── */
    /* Ghost "888" background — dim segments behind the real value */
    s_speed_ghost = lv_label_create(scr);
    lv_label_set_text(s_speed_ghost, "888");
    lv_obj_set_style_text_color(s_speed_ghost, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_text_font(s_speed_ghost, &font_dseg7_bold_270, 0);
    lv_obj_set_style_text_letter_space(s_speed_ghost, -8, 0);
    lv_obj_align(s_speed_ghost, LV_ALIGN_CENTER, 0, -50);

    /* Actual speed value — right-aligned to match ghost digits */
    s_speed_label = lv_label_create(scr);
    lv_label_set_text(s_speed_label, "0");
    lv_obj_set_style_text_color(s_speed_label, COL_TEXT_PRI, 0);
    lv_obj_set_style_text_font(s_speed_label, &font_dseg7_bold_270, 0);
    lv_obj_set_style_text_letter_space(s_speed_label, -8, 0);
    lv_obj_update_layout(s_speed_ghost);
    lv_obj_set_width(s_speed_label, lv_obj_get_width(s_speed_ghost));
    lv_obj_set_style_text_align(s_speed_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_speed_label, LV_ALIGN_CENTER, 0, -50);

    s_unit_label = lv_label_create(scr);
    lv_label_set_text(s_unit_label, "KM/H");
    lv_obj_set_style_text_color(s_unit_label, COL_TEXT_SEC, 0);
    lv_obj_set_style_text_font(s_unit_label, &font_montserrat_bold_72, 0);
    lv_obj_set_width(s_unit_label, lv_obj_get_width(s_speed_ghost));
    lv_obj_set_style_text_align(s_unit_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_unit_label, LV_ALIGN_CENTER, 0, 130);

    ESP_LOGI(TAG, "Speedometer UI created (800x480)");
}

void td5_display_update(const td5_live_data_t *data)
{
    // Status bar — use lv_label_set_text_static for string literals
    // to avoid 18,000+ needless lv_realloc cycles over 30 min.
    td5_state_t st = td5_usb_get_state();
    switch (st) {
        case TD5_STATE_IDLE:
            lv_label_set_text_static(s_status_label, "WAITING FOR USB CABLE...");
            lv_obj_set_style_text_color(s_status_label, COL_TEXT_DIM, 0);
            break;
        case TD5_STATE_INIT_SENT:
        case TD5_STATE_DIAG_STARTED:
        case TD5_STATE_SEED_REQUESTED:
            lv_label_set_text_static(s_status_label, "AUTHENTICATING...");
            lv_obj_set_style_text_color(s_status_label, COL_ACCENT, 0);
            break;
        case TD5_STATE_AUTHENTICATED:
        case TD5_STATE_POLLING:
            lv_label_set_text_static(s_status_label, "ECU CONNECTED");
            lv_obj_set_style_text_color(s_status_label, COL_ARC_GREEN, 0);
            break;
        case TD5_STATE_ERROR:
            lv_label_set_text_static(s_status_label, "ECU CONNECTION LOST!");
            lv_obj_set_style_text_color(s_status_label, COL_ARC_RED, 0);
            break;
    }

    if (!data || !data->valid) return;

    // Speed — write into persistent buffer and use lv_label_set_text_static
    // to completely avoid lv_realloc heap fragmentation.
    int spd = (int)(data->vehicle_speed_kph + 0.5f);
    if (spd < 0) spd = 0;
    if (spd > 999) spd = 999;
    snprintf(s_speed_buf, sizeof(s_speed_buf), "%d", spd);
    lv_label_set_text_static(s_speed_label, s_speed_buf);
    lv_obj_invalidate(s_speed_label);  /* force redraw since static text skips it */

}
