/*
 * TD5 Protocol - Land Rover TD5 ECU Diagnostic Protocol
 *
 * ISO 14230 (KWP2000) framing, K-line at 10400 baud, proprietary PIDs.
 *
 * References:
 *   - https://github.com/EA2EGA/Ekaitza_Itzali
 *   - https://github.com/BennehBoy/LRDuinoTD5
 *   - https://github.com/pajacobson/td5keygen
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol constants */
#define TD5_BAUD_RATE           10400
#define TD5_FAST_INIT_LOW_MS    25
#define TD5_FAST_INIT_HIGH_MS   25
#define TD5_TESTER_ADDR         0xF7
#define TD5_ECU_ADDR            0x13

/* Service IDs */
#define TD5_SID_START_DIAG      0x10
#define TD5_SID_SECURITY_ACCESS 0x27
#define TD5_SID_READ_DATA       0x21
#define TD5_SID_TESTER_PRESENT  0x3E

/* Sub-functions */
#define TD5_REQUEST_SEED        0x01
#define TD5_SEND_KEY            0x02
#define TD5_DIAG_SESSION        0xA0

/* TD5-specific PID request IDs (NOT standard OBD2)
 * Verified against real Defender TD5 ECU via K-line test log. */
#define TD5_PID_RPM             0x09
#define TD5_PID_SPEED           0x0D
#define TD5_PID_BATTERY         0x10
#define TD5_PID_THROTTLE        0x1A
#define TD5_PID_TEMPS           0x1C

/* Speed correction factor (GPS-verified: ECU reads ~5% low) */
#define TD5_SPEED_CORRECTION    1.05f

/* Throttle calibration: idle ADC ~928, WOT ~4096 */
#define TD5_THROTTLE_IDLE_ADC   900
#define TD5_THROTTLE_WOT_ADC    4096

/* Number of PIDs to poll each cycle */
#define TD5_POLL_PID_COUNT      5

/* Parsed live data */
typedef struct {
    uint16_t rpm;
    float    vehicle_speed_kph;
    float    coolant_temp_c;
    float    ambient_temp_c;
    float    battery_voltage;
    float    throttle_pct;
    float    inj_qty;
    bool     valid;
} td5_live_data_t;

/* Protocol state machine */
typedef enum {
    TD5_STATE_IDLE,
    TD5_STATE_INIT_SENT,
    TD5_STATE_DIAG_STARTED,
    TD5_STATE_SEED_REQUESTED,
    TD5_STATE_AUTHENTICATED,
    TD5_STATE_POLLING,
    TD5_STATE_ERROR,
} td5_state_t;

/* Message buffer */
#define TD5_MAX_MSG_LEN     128

typedef struct {
    uint8_t  data[TD5_MAX_MSG_LEN];
    uint8_t  len;
} td5_msg_t;

/* Message builders */
void td5_build_init_frame(td5_msg_t *msg);
void td5_build_start_diag(td5_msg_t *msg);
void td5_build_seed_request(td5_msg_t *msg);
void td5_build_key_response(td5_msg_t *msg, uint16_t seed);
void td5_build_pid_request(td5_msg_t *msg, uint8_t pid);
void td5_build_tester_present(td5_msg_t *msg);

/* Parsers */
esp_err_t td5_parse_pid(uint8_t pid, const uint8_t *response, uint8_t len,
                        td5_live_data_t *out);
bool td5_verify_checksum(const uint8_t *data, uint8_t len);
esp_err_t td5_extract_seed(const uint8_t *response, uint8_t len, uint16_t *seed_out);

#ifdef __cplusplus
}
#endif
