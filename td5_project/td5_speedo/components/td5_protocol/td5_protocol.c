/*
 * TD5 Protocol Implementation
 */
#include "td5_protocol.h"
#include "td5_keygen.h"
#include <string.h>

static uint8_t calc_checksum(const uint8_t *data, uint8_t len)
{
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) sum += data[i];
    return sum;
}

static void append_checksum(td5_msg_t *msg)
{
    msg->data[msg->len] = calc_checksum(msg->data, msg->len);
    msg->len++;
}

void td5_build_init_frame(td5_msg_t *msg)
{
    msg->len = 0;
    msg->data[msg->len++] = 0x81;          // Format: physical addr, 1 byte
    msg->data[msg->len++] = TD5_ECU_ADDR;  // 0x13
    msg->data[msg->len++] = TD5_TESTER_ADDR; // 0xF7
    msg->data[msg->len++] = 0x81;          // startCommunication
    append_checksum(msg);                   // 0x0C
}

void td5_build_start_diag(td5_msg_t *msg)
{
    msg->len = 0;
    msg->data[msg->len++] = 0x02;
    msg->data[msg->len++] = TD5_SID_START_DIAG;  // 0x10
    msg->data[msg->len++] = TD5_DIAG_SESSION;    // 0xA0
    append_checksum(msg);                         // 0xB2
}

void td5_build_seed_request(td5_msg_t *msg)
{
    msg->len = 0;
    msg->data[msg->len++] = 0x02;
    msg->data[msg->len++] = TD5_SID_SECURITY_ACCESS; // 0x27
    msg->data[msg->len++] = TD5_REQUEST_SEED;        // 0x01
    append_checksum(msg);                             // 0x2A
}

void td5_build_key_response(td5_msg_t *msg, uint16_t seed)
{
    uint16_t key = td5_keygen(seed);
    msg->len = 0;
    msg->data[msg->len++] = 0x04;
    msg->data[msg->len++] = TD5_SID_SECURITY_ACCESS; // 0x27
    msg->data[msg->len++] = TD5_SEND_KEY;            // 0x02
    msg->data[msg->len++] = (uint8_t)((key >> 8) & 0xFF); // high byte first
    msg->data[msg->len++] = (uint8_t)(key & 0xFF);        // low byte second
    append_checksum(msg);
}

void td5_build_pid_request(td5_msg_t *msg, uint8_t pid)
{
    msg->len = 0;
    msg->data[msg->len++] = 0x02;
    msg->data[msg->len++] = TD5_SID_READ_DATA; // 0x21
    msg->data[msg->len++] = pid;
    append_checksum(msg);
}

void td5_build_tester_present(td5_msg_t *msg)
{
    msg->len = 0;
    msg->data[msg->len++] = 0x02;
    msg->data[msg->len++] = TD5_SID_TESTER_PRESENT; // 0x3E
    msg->data[msg->len++] = 0x01;
    append_checksum(msg);
}

bool td5_verify_checksum(const uint8_t *data, uint8_t len)
{
    if (len < 2) return false;
    return (data[len - 1] == calc_checksum(data, len - 1));
}

esp_err_t td5_extract_seed(const uint8_t *response, uint8_t len, uint16_t *seed_out)
{
    // Expected: 0x04 0x67 0x01 [seed_hi] [seed_lo] [checksum]
    if (len < 6) return ESP_ERR_INVALID_SIZE;
    if (response[1] != 0x67 || response[2] != 0x01) return ESP_ERR_INVALID_RESPONSE;
    if (!td5_verify_checksum(response, len)) return ESP_ERR_INVALID_CRC;
    *seed_out = ((uint16_t)response[3] << 8) | response[4];
    return ESP_OK;
}

esp_err_t td5_parse_pid(uint8_t pid, const uint8_t *response, uint8_t len,
                        td5_live_data_t *out)
{
    if (len < 4) return ESP_ERR_INVALID_SIZE;
    if (response[1] != 0x61) return ESP_ERR_INVALID_RESPONSE;
    if (!td5_verify_checksum(response, len)) return ESP_ERR_INVALID_CRC;

    // Data payload starts at index 3 (len_byte, SID 0x61, PID echo, then data...)
    const uint8_t *d = &response[3];
    uint8_t data_len = len - 4; // subtract header(3) + checksum(1)

    switch (pid) {
    case TD5_PID_RPM: // 0x09 — 2 data bytes, direct 16-bit RPM
        if (data_len < 2) return ESP_ERR_INVALID_SIZE;
        out->rpm = ((uint16_t)d[0] << 8) | d[1];
        break;

    case TD5_PID_SPEED: { // 0x0D — 1 data byte, km/h with correction
        if (data_len < 1) return ESP_ERR_INVALID_SIZE;
        /* Stale-value detection: when the VSS goes silent at standstill the
         * ECU holds the last known speed byte.  If the raw value is low and
         * hasn't changed for several consecutive polls, treat it as 0. Real
         * motion always produces changing readings even at low speed. */
        static uint8_t prev_raw;
        static int     stale_count;
        uint8_t raw = d[0];
        if (raw == 0) {
            out->vehicle_speed_kph = 0.0f;
            stale_count = 0;
        } else if (raw <= 5 && raw == prev_raw) {
            stale_count++;
            out->vehicle_speed_kph = (stale_count >= 3) ? 0.0f
                                     : (float)raw * TD5_SPEED_CORRECTION;
        } else {
            stale_count = 0;
            out->vehicle_speed_kph = (float)raw * TD5_SPEED_CORRECTION;
        }
        prev_raw = raw;
        break;
    }

    case TD5_PID_BATTERY: // 0x10 — 4 data bytes (2 pairs), first pair / 1000 = volts
        if (data_len < 2) return ESP_ERR_INVALID_SIZE;
        out->battery_voltage = (float)(((uint16_t)d[0] << 8) | d[1]) / 1000.0f;
        break;

    case TD5_PID_THROTTLE: { // 0x1A — 16 data bytes (8 pairs)
        if (data_len < 10) return ESP_ERR_INVALID_SIZE;
        // Pair 1 (offset 2-3) = idle reference ADC for throttle %
        uint16_t idle_ref = ((uint16_t)d[2] << 8) | d[3];
        float pct = ((float)idle_ref - TD5_THROTTLE_IDLE_ADC)
                   / (TD5_THROTTLE_WOT_ADC - TD5_THROTTLE_IDLE_ADC) * 100.0f;
        out->throttle_pct = (pct < 0.0f) ? 0.0f : pct;
        // Pair 4 (offset 8-9) = injection quantity, /10 for mg/stroke
        uint16_t inj_raw = ((uint16_t)d[8] << 8) | d[9];
        out->inj_qty = (float)inj_raw / 10.0f;
        break;
    }

    case TD5_PID_TEMPS: // 0x1C — 8 data bytes (4 pairs)
        if (data_len < 8) return ESP_ERR_INVALID_SIZE;
        // Pair 2 (offset 4-5) = coolant temp °C direct
        out->coolant_temp_c = (float)(((uint16_t)d[4] << 8) | d[5]);
        // Pair 3 (offset 6-7) = ambient temp °C direct
        out->ambient_temp_c = (float)(((uint16_t)d[6] << 8) | d[7]);
        break;

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    return ESP_OK;
}
