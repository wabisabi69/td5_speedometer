#pragma once
#include "esp_err.h"
#include "td5_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t td5_usb_init(void);
esp_err_t td5_usb_connect(void);
td5_state_t td5_usb_get_state(void);
bool td5_usb_get_live_data(td5_live_data_t *out);

#ifdef __cplusplus
}
#endif
