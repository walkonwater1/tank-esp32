#include "time_sync.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tsp_mqtt_client.h"

#define TAG "time_sync"
#define TANK_TZ "CST-8"  // POSIX: China Standard Time = UTC+8
#define NTP_SERVER_PRIMARY "ntp.aliyun.com"
#define NTP_SERVER_FALLBACK "pool.ntp.org"
#define TIME_VALID_EPOCH 1700000000LL  // 2023-11-14

static bool time_is_valid(void)
{
    return (long long)time(NULL) >= TIME_VALID_EPOCH;
}

static void time_sync_task(void *arg)
{
    (void)arg;

    while (true) {
        bool connected = false;
        TankGetWifiStatus(NULL, 0, NULL, NULL, NULL, 0, &connected, NULL);
        if (connected) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Always ask SNTP, even if the cloud already supplied a timestamp. The
    // cloud time is only a fallback when NTP is unreachable.
    bool sntp_ok = false;
    int attempt = 0;
    while (true) {
        if (attempt > 0) {
            esp_sntp_stop();
        }
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, (attempt % 2 == 0) ? NTP_SERVER_PRIMARY : NTP_SERVER_FALLBACK);
        esp_sntp_init();

        for (int i = 0; i < 30; ++i) {
            if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                sntp_ok = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (sntp_ok) {
            break;
        }

        attempt++;
        if (time_is_valid() && attempt >= 2) {
            ESP_LOGW(TAG, "SNTP unavailable; using the cloud-supplied time");
            break;
        }
        ESP_LOGW(TAG, "SNTP not synced yet, retrying (%d)", attempt);
    }

    struct tm local = {0};
    TankTimeLocal(&local);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    ESP_LOGI(TAG, "time ready: %s (TZ=%s, sntp=%s)", buf, TANK_TZ,
             sntp_ok ? "yes" : "fallback");
    vTaskDelete(NULL);
}

void TankTimeInit(void)
{
    // Set TZ synchronously: the display clock and reminder parsing both use
    // localtime(). SNTP runs later, once Wi-Fi is up.
    setenv("TZ", TANK_TZ, 1);
    tzset();

    BaseType_t ret = xTaskCreate(time_sync_task, "time_sync", 3072, NULL, 3, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create time sync task");
    }
}

bool TankTimeIsSynced(void)
{
    return time_is_valid();
}

time_t TankTimeNow(void)
{
    return time(NULL);
}

bool TankTimeLocal(struct tm *out)
{
    if (out == NULL || !time_is_valid()) {
        return false;
    }
    time_t now = time(NULL);
    return localtime_r(&now, out) != NULL;
}
