// Host shim for ESP-IDF's esp_timer. The firmware's scr_dial.c uses
// esp_timer_get_time() for the boost/rail gesture timing; on the host we back it
// with a monotonic clock (microseconds since an arbitrary epoch), same contract
// the real esp_timer provides. Implemented in stubs.c.
#pragma once
#include <stdint.h>

int64_t esp_timer_get_time(void);
