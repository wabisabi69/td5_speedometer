#pragma once
#include "td5_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

void td5_display_create(void);
void td5_display_update(const td5_live_data_t *data);

#ifdef __cplusplus
}
#endif
