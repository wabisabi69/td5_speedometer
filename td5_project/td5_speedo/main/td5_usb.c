/*
 * TD5 USB Host Layer
 *
 * Uses ESP-IDF USB Host CDC-ACM VCP driver to talk to the
 * K+DCAN cable's FTDI FT232RL chip. Cable handles 12V K-line
 * level-shifting; we send/receive serial bytes through USB.
 *
 * IMPORTANT: Cable switch must be on K-LINE position (LEFT).
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

#include "td5_usb.h"
#include "td5_protocol.h"

static const char *TAG = "td5_usb";

static cdc_acm_dev_hdl_t s_cdc_dev = NULL;
static SemaphoreHandle_t s_rx_sem = NULL;
static uint8_t s_rx_buf[TD5_MAX_MSG_LEN];
static int s_rx_len = 0;
static int s_echo_skip = 0;          /* K-line echo bytes remaining to discard */
static td5_state_t s_state = TD5_STATE_IDLE;
static td5_live_data_t s_live_data = {0};
static SemaphoreHandle_t s_data_mutex = NULL;

/* ── FTDI vendor-specific USB control commands ───────────── */
/* FT232R/FT-X chips ignore standard CDC requests (SET_LINE_CODING etc.).
 * We must use FTDI's proprietary vendor requests via EP0 instead. */
#define FTDI_SIO_RESET              0x00
#define FTDI_SIO_SET_BAUDRATE       0x03
#define FTDI_SIO_SET_DATA           0x04
#define FTDI_SIO_SET_LATENCY_TIMER  0x09
#define FTDI_REQTYPE_OUT            0x40  /* host-to-device, vendor, device */
#define FTDI_RESET_SIO              0x00
#define FTDI_PURGE_RX               0x01
#define FTDI_PURGE_TX               0x02

/* FTDI fractional divisor encoding (index = fraction in eighths) */
static const uint8_t ftdi_frac_code[8] = {0, 3, 2, 4, 1, 5, 6, 7};

static esp_err_t ftdi_reset(void)
{
    return cdc_acm_host_send_custom_request(s_cdc_dev, FTDI_REQTYPE_OUT,
        FTDI_SIO_RESET, FTDI_RESET_SIO, 0, 0, NULL);
}

static esp_err_t ftdi_purge(void)
{
    cdc_acm_host_send_custom_request(s_cdc_dev, FTDI_REQTYPE_OUT,
        FTDI_SIO_RESET, FTDI_PURGE_RX, 0, 0, NULL);
    return cdc_acm_host_send_custom_request(s_cdc_dev, FTDI_REQTYPE_OUT,
        FTDI_SIO_RESET, FTDI_PURGE_TX, 0, 0, NULL);
}

static esp_err_t ftdi_set_baudrate(uint32_t baud)
{
    /* FT232R/FT-X base clock = 3 MHz (48 MHz / 16).
     * Divisor is computed in 1/8-unit resolution with FTDI's
     * non-linear fractional encoding packed into wValue/wIndex. */
    uint32_t div8 = (3000000 * 8 + baud / 2) / baud;
    uint16_t int_div  = div8 >> 3;
    uint8_t  frac_idx = div8 & 0x07;
    uint32_t encoded  = (uint32_t)int_div | ((uint32_t)ftdi_frac_code[frac_idx] << 14);
    uint16_t wValue = encoded & 0xFFFF;
    uint16_t wIndex = (encoded >> 16) & 0xFFFF;
    ESP_LOGI(TAG, "FTDI baud %lu  div=0x%04X idx=0x%04X", (unsigned long)baud, wValue, wIndex);
    return cdc_acm_host_send_custom_request(s_cdc_dev, FTDI_REQTYPE_OUT,
        FTDI_SIO_SET_BAUDRATE, wValue, wIndex, 0, NULL);
}

static esp_err_t ftdi_set_data(bool enable_break)
{
    /* wValue bits [7:0]=data_bits(8), [10:8]=parity(0=none),
     *              [12:11]=stop(0=1-stop), [14]=break */
    uint16_t wValue = 0x0008;  /* 8N1 */
    if (enable_break) wValue |= 0x4000;
    return cdc_acm_host_send_custom_request(s_cdc_dev, FTDI_REQTYPE_OUT,
        FTDI_SIO_SET_DATA, wValue, 0, 0, NULL);
}

static esp_err_t ftdi_set_latency(uint8_t ms)
{
    return cdc_acm_host_send_custom_request(s_cdc_dev, FTDI_REQTYPE_OUT,
        FTDI_SIO_SET_LATENCY_TIMER, ms, 0, 0, NULL);
}

/* ── USB Host daemon task ────────────────────────────────── */
static void usb_lib_task(void *arg)
{
    while (1) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

/* ── CDC-ACM data receive callback ───────────────────────── */
static bool handle_rx(const uint8_t *data, size_t data_len, void *arg)
{
    /* FTDI prepends 2 modem/line-status bytes to every USB packet.
     * The ESP-IDF CDC-ACM driver passes them through raw. Strip them. */
    if (data_len <= 2) return true;   /* status-only packet, no serial data */
    data     += 2;
    data_len -= 2;

    /* K-line is a single wire: our transmitted bytes echo back.
     * Discard the expected echo before looking for the ECU response. */
    if (s_echo_skip > 0) {
        size_t skip = (data_len <= (size_t)s_echo_skip) ? data_len : (size_t)s_echo_skip;
        s_echo_skip -= (int)skip;
        data     += skip;
        data_len -= skip;
    }

    if (data_len > 0 && s_rx_len + (int)data_len <= TD5_MAX_MSG_LEN) {
        memcpy(&s_rx_buf[s_rx_len], data, data_len);
        s_rx_len += (int)data_len;
    }

    /* Check for a complete message */
    if (s_rx_len > 0) {
        int expected;
        if (s_rx_buf[0] >= 0x80) {
            /* ISO 14230 header: [fmt] [tgt] [src] [payload…] [cs]
             * payload length is in the low 6 bits of the format byte */
            expected = (s_rx_buf[0] & 0x3F) + 4;
        } else {
            /* Short format: [length] [payload…] [cs] */
            expected = s_rx_buf[0] + 2;
        }
        if (s_rx_len >= expected) {
            xSemaphoreGive(s_rx_sem);
        }
    }
    return true;
}

/* ── CDC-ACM device event callback ───────────────────────── */
static void handle_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    if (event->type == CDC_ACM_HOST_DEVICE_DISCONNECTED) {
        ESP_LOGW(TAG, "USB device disconnected");
        s_state = TD5_STATE_ERROR;
        if (s_cdc_dev) {
            cdc_acm_host_close(s_cdc_dev);
            s_cdc_dev = NULL;
        }
    }
}

/* ── Send/Receive helpers ────────────────────────────────── */
static esp_err_t td5_send(const td5_msg_t *msg)
{
    if (!s_cdc_dev) return ESP_ERR_INVALID_STATE;
    s_rx_len = 0;
    s_echo_skip = msg->len;              /* K-line echo: discard our own TX */
    xSemaphoreTake(s_rx_sem, 0);         /* clear pending */
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, msg->data, msg->len, ESP_LOG_DEBUG);
    return cdc_acm_host_data_tx_blocking(s_cdc_dev, msg->data, msg->len, 1000);
}

static esp_err_t td5_recv(uint8_t *buf, int *len, int timeout_ms)
{
    if (xSemaphoreTake(s_rx_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "RX timeout (state=%d)", s_state);
        return ESP_ERR_TIMEOUT;
    }
    memcpy(buf, s_rx_buf, s_rx_len);
    *len = s_rx_len;
    ESP_LOG_BUFFER_HEX_LEVEL(TAG, buf, *len, ESP_LOG_DEBUG);
    return ESP_OK;
}

/* ── Fast Init (25ms BREAK on K-line) ────────────────────── */
static esp_err_t td5_fast_init(void)
{
    ESP_LOGI(TAG, "Fast Init: sending 25ms BREAK via FTDI...");

    /* Pull K-line low for 25 ms using FTDI's hardware break */
    esp_err_t ret = ftdi_set_data(true);   /* Break ON (8N1 + break bit) */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FTDI break-on failed");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(TD5_FAST_INIT_LOW_MS));

    ftdi_set_data(false);                  /* Break OFF (8N1, normal) */

    /* Wait for K-line to return high */
    vTaskDelay(pdMS_TO_TICKS(TD5_FAST_INIT_HIGH_MS));

    /* Set baud rate with FTDI vendor command */
    ret = ftdi_set_baudrate(TD5_BAUD_RATE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set FTDI baud to %d", TD5_BAUD_RATE);
        return ret;
    }

    /* Flush any stale data from FTDI FIFOs */
    ftdi_purge();
    s_rx_len = 0;
    s_echo_skip = 0;

    ESP_LOGI(TAG, "Fast Init done, baud=%d", TD5_BAUD_RATE);
    return ESP_OK;
}

/* ── Full handshake: init → start diag → seed/key auth ───── */
static esp_err_t td5_handshake(void)
{
    td5_msg_t msg;
    uint8_t resp[TD5_MAX_MSG_LEN];
    int rlen;
    esp_err_t ret;

    // 1) Fast Init
    ret = td5_fast_init();
    if (ret != ESP_OK) return ret;

    // 2) Init Frame
    ESP_LOGI(TAG, "Sending init frame...");
    td5_build_init_frame(&msg);
    ret = td5_send(&msg);
    if (ret != ESP_OK) return ret;
    ret = td5_recv(resp, &rlen, 2000);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "No init response"); return ret; }
    ESP_LOGI(TAG, "Init response OK (%d bytes)", rlen);
    s_state = TD5_STATE_INIT_SENT;
    vTaskDelay(pdMS_TO_TICKS(55));

    // 3) Start Diagnostics
    ESP_LOGI(TAG, "Start diagnostic session...");
    td5_build_start_diag(&msg);
    ret = td5_send(&msg);
    if (ret != ESP_OK) return ret;
    ret = td5_recv(resp, &rlen, 2000);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "No start-diag response"); return ret; }
    if (rlen >= 2 && resp[1] == 0x50) {
        ESP_LOGI(TAG, "Diagnostic session started");
        s_state = TD5_STATE_DIAG_STARTED;
    } else {
        ESP_LOGE(TAG, "Bad start-diag response");
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(55));

    // 4) Request Seed
    ESP_LOGI(TAG, "Requesting seed...");
    td5_build_seed_request(&msg);
    ret = td5_send(&msg);
    if (ret != ESP_OK) return ret;
    ret = td5_recv(resp, &rlen, 2000);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "No seed response"); return ret; }

    uint16_t seed;
    ret = td5_extract_seed(resp, rlen, &seed);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Bad seed"); return ret; }
    ESP_LOGI(TAG, "Seed: 0x%04X", seed);
    s_state = TD5_STATE_SEED_REQUESTED;
    vTaskDelay(pdMS_TO_TICKS(55));

    // 5) Send Key
    ESP_LOGI(TAG, "Sending key...");
    td5_build_key_response(&msg, seed);
    ret = td5_send(&msg);
    if (ret != ESP_OK) return ret;
    ret = td5_recv(resp, &rlen, 2000);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "No key response"); return ret; }

    if (rlen >= 3 && resp[1] == 0x67 && resp[2] == 0x02) {
        ESP_LOGI(TAG, "*** AUTHENTICATED ***");
        s_state = TD5_STATE_AUTHENTICATED;
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Authentication FAILED");
    s_state = TD5_STATE_ERROR;
    return ESP_FAIL;
}

/* ── Main polling task ───────────────────────────────────── */
static void td5_poll_task(void *arg)
{
    td5_msg_t msg;
    uint8_t resp[TD5_MAX_MSG_LEN];
    int rlen;
    int keepalive = 0;

    while (1) {
        /* ── Handshake phase: retry until authenticated ──── */
        s_state = TD5_STATE_IDLE;
        bool authenticated = false;

        for (int i = 0; i < 5; i++) {
            ESP_LOGI(TAG, "Handshake attempt %d/5...", i + 1);

            /* Flush stale data before each attempt */
            ftdi_purge();
            s_rx_len = 0;
            s_echo_skip = 0;
            xSemaphoreTake(s_rx_sem, 0);  /* clear pending semaphore */

            if (td5_handshake() == ESP_OK) {
                authenticated = true;
                break;
            }
            ESP_LOGW(TAG, "Retrying in 2s...");
            s_state = TD5_STATE_IDLE;     /* show "CONNECTING" not "ERROR" between retries */
            vTaskDelay(pdMS_TO_TICKS(2000));
        }

        if (!authenticated) {
            ESP_LOGE(TAG, "Could not authenticate after 5 attempts, retrying in 5s...");
            s_state = TD5_STATE_ERROR;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;  /* restart the outer loop — try handshake again */
        }

        /* Brief settle time after auth — K-line needs an inter-message
         * gap before the ECU will accept PID requests. Also flush any
         * stale FTDI data so the first poll starts clean. */
        vTaskDelay(pdMS_TO_TICKS(100));
        ftdi_purge();
        s_rx_len = 0;
        s_echo_skip = 0;
        xSemaphoreTake(s_rx_sem, 0);

        s_state = TD5_STATE_POLLING;
        keepalive = 0;
        int poll_errors = 0;
        int heap_check = 0;

        /* ── Polling phase ──────────────────────────────── */
        while (s_state == TD5_STATE_POLLING) {
            td5_build_pid_request(&msg, TD5_PID_SPEED);
            if (td5_send(&msg) != ESP_OK) { s_state = TD5_STATE_ERROR; break; }
            if (td5_recv(resp, &rlen, 1000) == ESP_OK) {
                poll_errors = 0;
                xSemaphoreTake(s_data_mutex, portMAX_DELAY);
                td5_parse_pid(TD5_PID_SPEED, resp, rlen, &s_live_data);
                s_live_data.valid = true;
                xSemaphoreGive(s_data_mutex);
            } else {
                poll_errors++;
                if (poll_errors >= 3) {
                    /* Multiple consecutive timeouts — connection is truly lost */
                    ESP_LOGW(TAG, "Speed poll timeout (%d consecutive), reconnecting...", poll_errors);
                    s_state = TD5_STATE_ERROR;
                    break;
                }
                ESP_LOGW(TAG, "Speed poll timeout (%d/%d), retrying...", poll_errors, 3);
                ftdi_purge();
                s_rx_len = 0;
                s_echo_skip = 0;
                xSemaphoreTake(s_rx_sem, 0);
            }

            if (++keepalive >= 20) {
                keepalive = 0;
                td5_build_tester_present(&msg);
                td5_send(&msg);
                td5_recv(resp, &rlen, 500);

                /* Inter-message gap: flush stale FTDI data so the
                 * tester-present response doesn't bleed into the next
                 * speed request. */
                vTaskDelay(pdMS_TO_TICKS(55));
                ftdi_purge();
                s_rx_len = 0;
                s_echo_skip = 0;
                xSemaphoreTake(s_rx_sem, 0);
            }

            vTaskDelay(pdMS_TO_TICKS(200)); // 5 Hz polling rate

            /* Periodic heap health check — log every ~30s so we can
             * spot fragmentation or leaks long before they crash. */
            if (++heap_check >= 150) {   /* 150 × 200ms ≈ 30s */
                heap_check = 0;
                ESP_LOGI(TAG, "Heap: free=%lu  min_ever=%lu  largest_block=%lu",
                    (unsigned long)esp_get_free_heap_size(),
                    (unsigned long)esp_get_minimum_free_heap_size(),
                    (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
            }
        }

        /* Polling broke out — show error briefly then retry */
        ESP_LOGW(TAG, "Polling lost, reconnecting in 3s...");
        s_state = TD5_STATE_ERROR;
        s_live_data.valid = false;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

/* ── Public API ──────────────────────────────────────────── */

esp_err_t td5_usb_init(void)
{
    s_rx_sem = xSemaphoreCreateBinary();
    s_data_mutex = xSemaphoreCreateMutex();

    usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));

    ESP_LOGI(TAG, "USB Host ready");
    return ESP_OK;
}

esp_err_t td5_usb_connect(void)
{
    ESP_LOGI(TAG, "Opening FTDI device (0x0403:0x6001)...");

    const cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 10000,
        .out_buffer_size = 64,
        .in_buffer_size = 64,      /* = FTDI max-packet-size → one packet per callback */
        .event_cb = handle_event,
        .data_cb = handle_rx,
        .user_arg = NULL,
    };

    esp_err_t ret = cdc_acm_host_open_vendor_specific(
        0x0403, 0x6001, 0, &dev_cfg, &s_cdc_dev);

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Trying PID 0x6015...");
        ret = cdc_acm_host_open_vendor_specific(
            0x0403, 0x6015, 0, &dev_cfg, &s_cdc_dev);
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FTDI not found. Check USB OTG port & OTG adapter.");
        return ret;
    }

    ESP_LOGI(TAG, "FTDI opened OK — configuring via vendor commands");

    /* Reset FTDI and configure with vendor-specific USB requests
     * (standard CDC SET_LINE_CODING / SEND_BREAK are ignored by FTDI chips) */
    ftdi_reset();
    vTaskDelay(pdMS_TO_TICKS(50));
    ftdi_set_data(false);                   /* 8N1, no break */
    ftdi_set_baudrate(TD5_BAUD_RATE);       /* 10400 baud */
    ftdi_set_latency(2);                    /* 2 ms latency → fast response */
    ftdi_purge();

    xTaskCreate(td5_poll_task, "td5_poll", 8192, NULL, 5, NULL);
    return ESP_OK;
}

td5_state_t td5_usb_get_state(void)
{
    return s_state;
}

bool td5_usb_get_live_data(td5_live_data_t *out)
{
    if (s_state != TD5_STATE_POLLING) return false;
    xSemaphoreTake(s_data_mutex, portMAX_DELAY);
    memcpy(out, &s_live_data, sizeof(td5_live_data_t));
    xSemaphoreGive(s_data_mutex);
    return out->valid;
}
