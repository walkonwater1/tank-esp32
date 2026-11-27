#pragma once

#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Set the local timezone and start SNTP after Wi-Fi comes up.
void TankTimeInit(void);

// True once the wall clock is plausible (SNTP or an HTTP Date has set it).
bool TankTimeIsSynced(void);

time_t TankTimeNow(void);

// localtime_r with the configured timezone. Returns false if the clock is not set.
bool TankTimeLocal(struct tm *out);

#ifdef __cplusplus
}
#endif
