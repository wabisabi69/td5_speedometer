/*
 * TD5 Speedometer - Land Rover Defender TD5
 * ESP32-P4 + ESP32-C6, 4.3" 480×800 IPS, K+DCAN USB cable
 *
 * Display init uses the manufacturer's BSP (esp32_p4_function_ev_board)
 * which handles MIPI-DSI, ST7701 panel, GT911 touch, and LVGL integration.
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"

/* Board Support Package from common_components */
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "td5_usb.h"
#include "td5_display.h"
#include "td5_protocol.h"

static const char *TAG = "td5_main";

/* ── Display update timer callback ───────────────────────── */
static void display_timer_cb(lv_timer_t *timer)
{
    td5_live_data_t data = {0};
    td5_usb_get_live_data(&data);
    td5_display_update(&data);
}

/* ── Application Entry Point ─────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "╔═══════════════════════════════════════╗");
    ESP_LOGI(TAG, "║   TD5 Speedometer - Defender Edition  ║");
    ESP_LOGI(TAG, "║   ESP32-P4 + K+DCAN USB Interface     ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════╝");

    /* ── Step 1: Init display using manufacturer BSP ─────── */
    /* This matches the manufacturer's lvgl_demo_v9 exactly. */
    ESP_LOGI(TAG, "Initialising display via BSP...");

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = 480 * 800,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = true,
        }
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    ESP_LOGI(TAG, "Display started");

    /* ── Step 2: Create speedometer UI ───────────────────── */
    /* Lock LVGL mutex (BSP provides thread-safe wrapper) */
    bsp_display_lock(0);

    /* Rotate display to landscape (800×480), flipped edge */
    lv_display_set_rotation(lv_display_get_default(), LV_DISPLAY_ROTATION_270);

    td5_display_create();

    /* Register a periodic timer to update the display with ECU data.
     * This runs inside LVGL's own task managed by the BSP. */
    lv_timer_create(display_timer_cb, 100, NULL);  /* 100ms = 10 Hz update */
    bsp_display_unlock();

    bsp_display_brightness_set(100);

    /* ── Step 3: Init USB Host for K+DCAN cable ──────────── */
    ESP_LOGI(TAG, "Initialising USB Host...");
    ESP_ERROR_CHECK(td5_usb_init());
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* ── Step 4: Connect to cable and start ECU polling ──── */
    ESP_LOGI(TAG, "Connecting to K+DCAN cable...");
    esp_err_t ret = td5_usb_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "K+DCAN cable not found. Check:");
        ESP_LOGE(TAG, "  - Plugged into USB OTG port (not UART port)");
        ESP_LOGE(TAG, "  - USB-C OTG adapter connected");
        ESP_LOGE(TAG, "  - Cable switch on K-LINE (left)");
        ESP_LOGE(TAG, "  - Vehicle ignition ON");
        /* Display will show error status via the timer callback */
    }

    ESP_LOGI(TAG, "TD5 Speedometer running!");

    /* The BSP's LVGL port runs its own task, and our td5_poll_task
     * runs separately. app_main can return - FreeRTOS keeps going. */
}
