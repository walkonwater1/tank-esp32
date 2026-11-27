/*
 * micro-ROS UDP bridge for the Yuyi A2 tank.
 *
 * The existing Wi-Fi station is owned by the board firmware; this task only
 * waits for it to come up and then connects to the micro-ROS agent.  It does
 * not call uros_network_interface_initialize(), because that would initialize
 * Wi-Fi and NVS a second time.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include <rcl/rcl.h>
#include <rcl/error_handling.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>
#include <rmw_microros/rmw_microros.h>

#include <geometry_msgs/msg/twist.h>
#include <sensor_msgs/msg/battery_state.h>
#include <std_msgs/msg/string.h>

#include "microros_node.h"
#include "tank_control.h"
#include "tsp_mqtt_client.h"

#define TAG "microros"

#define RCCHECK(fn)                                                         \
    do {                                                                    \
        rcl_ret_t rc = (fn);                                                \
        if (rc != RCL_RET_OK) {                                             \
            ESP_LOGE(TAG, "failed at line %d: %d", __LINE__, (int)rc);      \
            vTaskDelete(NULL);                                              \
        }                                                                   \
    } while (0)

#define RCSOFTCHECK(fn)                                                     \
    do {                                                                    \
        rcl_ret_t rc = (fn);                                                \
        if (rc != RCL_RET_OK) {                                             \
            ESP_LOGW(TAG, "soft failure at line %d: %d", __LINE__, (int)rc);\
        }                                                                   \
    } while (0)

static rcl_publisher_t s_battery_pub;
static rcl_publisher_t s_status_pub;
static rcl_subscription_t s_cmd_vel_sub;
static rcl_timer_t s_status_timer;

static geometry_msgs__msg__Twist s_cmd_vel_msg;
static sensor_msgs__msg__BatteryState s_battery_msg;
static std_msgs__msg__String s_status_msg;
static char s_status_buf[256];

static TickType_t s_last_cmd_tick;

static void cmd_vel_callback(const void *msgin)
{
    const geometry_msgs__msg__Twist *msg = (const geometry_msgs__msg__Twist *)msgin;
    if (msg == NULL) {
        return;
    }

    if (!TankSetVelocity((float)msg->linear.x, (float)msg->angular.z)) {
        ESP_LOGW(TAG, "cmd_vel rejected (motion queue full)");
        return;
    }
    s_last_cmd_tick = xTaskGetTickCount();
}

static void publish_status(void)
{
    bool connected = false;
    int rssi = 0;
    int channel = 0;
    char ip[16] = {0};
    TankGetWifiStatus(NULL, 0, &rssi, &channel, ip, sizeof(ip), &connected, NULL);

    const unsigned uptime_s = (unsigned)(esp_timer_get_time() / 1000000);
    const unsigned free_heap = (unsigned)esp_get_free_heap_size();

    int len = snprintf(s_status_buf, sizeof(s_status_buf),
                       "{\"wifi\":{\"connected\":%s,\"rssi\":%d,\"channel\":%d,"
                       "\"ip\":\"%s\"},\"free_heap\":%u,\"uptime_s\":%u}",
                       connected ? "true" : "false", rssi, channel, ip,
                       free_heap, uptime_s);
    if (len < 0) {
        return;
    }
    if (len >= (int)sizeof(s_status_buf)) {
        len = (int)sizeof(s_status_buf) - 1;
    }

    s_status_msg.data.data = s_status_buf;
    s_status_msg.data.size = (size_t)len;
    s_status_msg.data.capacity = sizeof(s_status_buf);
    RCSOFTCHECK(rcl_publish(&s_status_pub, &s_status_msg, NULL));
}

static void status_timer_callback(rcl_timer_t *timer, int64_t last_call_time)
{
    (void)last_call_time;
    if (timer == NULL) {
        return;
    }

    int voltage_mv = -1;
    int percentage = -1;
    int charge_pin = -1;
    int done_pin = -1;
    bool charging = false;
    bool discharging = false;
    bool full = false;
    TankGetPowerStatus(&voltage_mv, &percentage, &charging, &discharging,
                       &full, &charge_pin, &done_pin);

    s_battery_msg.voltage = voltage_mv >= 0 ? voltage_mv / 1000.0f : NAN;
    s_battery_msg.temperature = NAN;
    s_battery_msg.current = NAN;
    s_battery_msg.charge = NAN;
    s_battery_msg.capacity = NAN;
    s_battery_msg.design_capacity = NAN;
    s_battery_msg.percentage = percentage >= 0 ? percentage / 100.0f : NAN;
    if (charging) {
        s_battery_msg.power_supply_status =
            sensor_msgs__msg__BatteryState__POWER_SUPPLY_STATUS_CHARGING;
    } else if (discharging) {
        s_battery_msg.power_supply_status =
            sensor_msgs__msg__BatteryState__POWER_SUPPLY_STATUS_DISCHARGING;
    } else if (full) {
        s_battery_msg.power_supply_status =
            sensor_msgs__msg__BatteryState__POWER_SUPPLY_STATUS_FULL;
    } else {
        s_battery_msg.power_supply_status =
            sensor_msgs__msg__BatteryState__POWER_SUPPLY_STATUS_UNKNOWN;
    }
    s_battery_msg.power_supply_health =
        sensor_msgs__msg__BatteryState__POWER_SUPPLY_HEALTH_UNKNOWN;
    s_battery_msg.power_supply_technology =
        sensor_msgs__msg__BatteryState__POWER_SUPPLY_TECHNOLOGY_LIMN;
    s_battery_msg.present = true;

    RCSOFTCHECK(rcl_publish(&s_battery_pub, &s_battery_msg, NULL));
    publish_status();
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

static void microros_task(void *arg)
{
    (void)arg;
    wait_for_wifi();

    ESP_LOGI(TAG, "Wi-Fi ready, connecting to agent %s:%s",
             CONFIG_MICRO_ROS_AGENT_IP, CONFIG_MICRO_ROS_AGENT_PORT);

    rcl_allocator_t allocator = rcl_get_default_allocator();
    rclc_support_t support;

    rcl_init_options_t init_options = rcl_get_zero_initialized_init_options();
    RCCHECK(rcl_init_options_init(&init_options, allocator));

    rmw_init_options_t *rmw_options =
        rcl_init_options_get_rmw_init_options(&init_options);
    RCCHECK(rmw_uros_options_set_udp_address(CONFIG_MICRO_ROS_AGENT_IP,
                                             CONFIG_MICRO_ROS_AGENT_PORT,
                                             rmw_options));

    RCCHECK(rclc_support_init_with_options(&support, 0, NULL, &init_options,
                                           &allocator));

    rcl_node_t node;
    RCCHECK(rclc_node_init_default(&node, "esp32_tank", "", &support));

    // Sensor messages contain strings; micro-ROS serializes them from these
    // static buffers, so keep them non-NULL for the lifetime of the node.
    static char frame_id[] = "base_link";
    static char empty_string[] = "";
    s_battery_msg.header.frame_id.data = frame_id;
    s_battery_msg.header.frame_id.size = strlen(frame_id);
    s_battery_msg.header.frame_id.capacity = sizeof(frame_id);
    s_battery_msg.location.data = empty_string;
    s_battery_msg.location.size = 0;
    s_battery_msg.location.capacity = sizeof(empty_string);
    s_battery_msg.serial_number.data = empty_string;
    s_battery_msg.serial_number.size = 0;
    s_battery_msg.serial_number.capacity = sizeof(empty_string);

    RCCHECK(rclc_publisher_init_default(
        &s_battery_pub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(sensor_msgs, msg, BatteryState),
        "battery_state"));

    RCCHECK(rclc_publisher_init_default(
        &s_status_pub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, String), "tank/status"));

    // Best-effort accepts both best-effort teleop publishers and the default
    // reliable publishers without a QoS mismatch.
    RCCHECK(rclc_subscription_init_best_effort(
        &s_cmd_vel_sub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Twist), "cmd_vel"));

    RCCHECK(rclc_timer_init_default(&s_status_timer, &support, RCL_MS_TO_NS(1000),
                                    status_timer_callback));

    rclc_executor_t executor;
    RCCHECK(rclc_executor_init(&executor, &support.context, 2, &allocator));
    RCCHECK(rclc_executor_add_subscription(&executor, &s_cmd_vel_sub,
                                           &s_cmd_vel_msg, &cmd_vel_callback,
                                           ON_NEW_DATA));
    RCCHECK(rclc_executor_add_timer(&executor, &s_status_timer));

    ESP_LOGI(TAG, "micro-ROS node ready: /cmd_vel, /battery_state, /tank/status");

    while (true) {
        rclc_executor_spin_some(&executor, RCL_MS_TO_NS(100));

        if (s_last_cmd_tick != 0 &&
            (xTaskGetTickCount() - s_last_cmd_tick) > pdMS_TO_TICKS(500)) {
            ESP_LOGW(TAG, "cmd_vel timeout, stopping");
            TankStop();
            s_last_cmd_tick = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void MicrorosStart(void)
{
    // 10 KB: after the agent connection the high-water mark leaves ~7.8 KB free.
    BaseType_t ret = xTaskCreatePinnedToCore(microros_task, "microros", 10240,
                                             NULL, 5, NULL, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "failed to create micro-ROS task");
    }
}
