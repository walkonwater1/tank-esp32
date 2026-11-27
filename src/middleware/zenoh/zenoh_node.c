/*
 * Zenoh-pico backend for the Yuyi A2 tank.
 *
 * This is the alternative to the micro-ROS backend in middleware/microros/.
 * The two are selected at compile time with CONFIG_ROBOT_MIDDLEWARE_*.
 *
 * Keys:
 *   tank/cmd_vel        {"linear": m/s, "angular": rad/s}
 *   tank/battery_state  JSON snapshot
 *   tank/status         Wi-Fi / heap / uptime JSON
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "cJSON.h"
#include "zenoh-pico.h"

#include "zenoh_node.h"
#include "tank_control.h"
#include "tsp_mqtt_client.h"

#define TAG "zenoh"

static z_owned_publisher_t s_battery_pub;
static z_owned_publisher_t s_status_pub;
static z_owned_subscriber_t s_cmd_vel_sub;

static TickType_t s_last_cmd_tick;

static void cmd_vel_callback(z_loaned_sample_t *sample, void *arg)
{
    (void)arg;
    z_owned_string_t payload;
    if (z_bytes_to_string(z_sample_payload(sample), &payload) < 0) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(z_string_data(z_loan(payload)),
                                        z_string_len(z_loan(payload)));
    z_string_drop(z_move(payload));
    if (root == NULL) {
        return;
    }

    cJSON *linear = cJSON_GetObjectItemCaseSensitive(root, "linear");
    cJSON *angular = cJSON_GetObjectItemCaseSensitive(root, "angular");
    if (cJSON_IsNumber(linear) && cJSON_IsNumber(angular)) {
        if (TankSetVelocity((float)linear->valuedouble,
                            (float)angular->valuedouble)) {
            s_last_cmd_tick = xTaskGetTickCount();
        }
    }
    cJSON_Delete(root);
}

static bool wait_for_wifi(void)
{
    while (true) {
        bool connected = false;
        TankGetWifiStatus(NULL, 0, NULL, NULL, NULL, 0, &connected, NULL);
        if (connected) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void publish_text(z_owned_publisher_t *pub, const char *text, size_t len)
{
    z_owned_bytes_t payload;
    if (z_bytes_copy_from_str(&payload, text) < 0) {
        return;
    }
    z_publisher_put(z_loan(*pub), z_move(payload), NULL);
    (void)len;
}

static void publish_battery(void)
{
    int voltage_mv = -1;
    int percentage = -1;
    bool charging = false;
    bool discharging = false;
    bool full = false;
    TankGetPowerStatus(&voltage_mv, &percentage, &charging, &discharging,
                       &full, NULL, NULL);

    char buf[192];
    int len = snprintf(buf, sizeof(buf),
                       "{\"voltage_mv\":%d,\"percentage\":%d,"
                       "\"charging\":%s,\"discharging\":%s,\"full\":%s}",
                       voltage_mv, percentage, charging ? "true" : "false",
                       discharging ? "true" : "false", full ? "true" : "false");
    if (len > 0 && len < (int)sizeof(buf)) {
        publish_text(&s_battery_pub, buf, (size_t)len);
    }
}

static void publish_status(void)
{
    bool connected = false;
    int rssi = 0;
    int channel = 0;
    char ip[16] = {0};
    TankGetWifiStatus(NULL, 0, &rssi, &channel, ip, sizeof(ip), &connected, NULL);

    char buf[256];
    int len = snprintf(buf, sizeof(buf),
                       "{\"wifi\":{\"connected\":%s,\"rssi\":%d,\"channel\":%d,"
                       "\"ip\":\"%s\"},\"free_heap\":%u,\"uptime_s\":%u}",
                       connected ? "true" : "false", rssi, channel, ip,
                       (unsigned)esp_get_free_heap_size(),
                       (unsigned)(esp_timer_get_time() / 1000000));
    if (len > 0 && len < (int)sizeof(buf)) {
        publish_text(&s_status_pub, buf, (size_t)len);
    }
}

static bool open_session(z_owned_session_t *session)
{
    z_owned_config_t config;
    if (z_config_default(&config) < 0) {
        return false;
    }
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, "client");
    if (zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY,
                         CONFIG_ZENOH_CONNECT_ENDPOINT) < 0) {
        z_drop(z_move(config));
        return false;
    }
    if (z_open(session, z_move(config), NULL) < 0) {
        return false;
    }
    return true;
}

static void zenoh_task(void *arg)
{
    (void)arg;
    wait_for_wifi();

    while (true) {
        z_owned_session_t session;
        if (!open_session(&session)) {
            ESP_LOGW(TAG, "cannot open Zenoh session to %s",
                     CONFIG_ZENOH_CONNECT_ENDPOINT);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        z_view_keyexpr_t battery_ke;
        z_view_keyexpr_t status_ke;
        z_view_keyexpr_t cmd_ke;
        z_view_keyexpr_from_str_unchecked(&battery_ke, "tank/battery_state");
        z_view_keyexpr_from_str_unchecked(&status_ke, "tank/status");
        z_view_keyexpr_from_str_unchecked(&cmd_ke, "tank/cmd_vel");

        if (z_declare_publisher(z_loan(session), &s_battery_pub,
                                z_loan(battery_ke), NULL) < 0 ||
            z_declare_publisher(z_loan(session), &s_status_pub,
                                z_loan(status_ke), NULL) < 0) {
            ESP_LOGE(TAG, "failed to declare Zenoh publisher");
            z_drop(z_move(session));
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        z_owned_closure_sample_t callback;
        z_closure(&callback, cmd_vel_callback, NULL, NULL);
        if (z_declare_subscriber(z_loan(session), &s_cmd_vel_sub,
                                 z_loan(cmd_ke), z_move(callback), NULL) < 0) {
            ESP_LOGE(TAG, "failed to declare Zenoh subscriber");
            z_drop(z_move(s_battery_pub));
            z_drop(z_move(s_status_pub));
            z_drop(z_move(session));
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        ESP_LOGI(TAG, "Zenoh session ready: %s", CONFIG_ZENOH_CONNECT_ENDPOINT);
        TickType_t last_publish = 0;

        while (true) {
            TickType_t now = xTaskGetTickCount();
            if (now - last_publish >= pdMS_TO_TICKS(1000)) {
                last_publish = now;
                publish_battery();
                publish_status();
            }

            if (s_last_cmd_tick != 0 &&
                (now - s_last_cmd_tick) > pdMS_TO_TICKS(500)) {
                ESP_LOGW(TAG, "cmd_vel timeout, stopping");
                TankStop();
                s_last_cmd_tick = 0;
            }

            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

void ZenohStart(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(zenoh_task, "zenoh", 16384, NULL, 5,
                                             NULL, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create Zenoh task");
    }
}
