#include "reminder_manager.h"

#include <cstring>
#include <ctime>

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "time_sync.h"

#define TAG "reminder"

namespace {
constexpr const char* kNvsNamespace = "reminder";
constexpr const char* kNvsKey = "items";
}  // namespace

ReminderManager& ReminderManager::GetInstance()
{
    static ReminderManager instance;
    return instance;
}

void ReminderManager::Init()
{
    if (initialized_) {
        return;
    }

    lock_ = xSemaphoreCreateMutex();
    notify_queue_ = xQueueCreate(kMaxReminders, sizeof(Notice));
    if (lock_ == nullptr || notify_queue_ == nullptr) {
        ESP_LOGE(TAG, "failed to allocate reminder primitives");
        return;
    }

    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_handle_ = handle;
        nvs_ok_ = true;
        LoadLocked();
    } else {
        ESP_LOGW(TAG, "NVS unavailable; reminders will not survive reboot");
    }

    xTaskCreate(ManagerTask, "reminder", 4096, this, 2, nullptr);
    xTaskCreate(NotifyTask, "reminder_notify", 4096, this, 2, nullptr);
    initialized_ = true;
    ESP_LOGI(TAG, "reminder manager ready (%u saved)", (unsigned)count_);
}

void ReminderManager::LoadLocked()
{
    if (!nvs_ok_) {
        return;
    }
    Persist p = {};
    size_t len = sizeof(p);
    esp_err_t err = nvs_get_blob((nvs_handle_t)nvs_handle_, kNvsKey, &p, &len);
    if (err != ESP_OK || len != sizeof(p) || p.magic != kMagic ||
        p.count > kMaxReminders || p.next_id == 0) {
        count_ = 0;
        next_id_ = 1;
        return;
    }
    count_ = p.count;
    next_id_ = p.next_id;
    std::memcpy(items_, p.items, sizeof(items_));
}

bool ReminderManager::SaveLocked()
{
    if (!nvs_ok_) {
        return false;
    }
    Persist p = {};
    p.magic = kMagic;
    p.count = (uint32_t)count_;
    p.next_id = next_id_;
    std::memcpy(p.items, items_, sizeof(items_));
    if (nvs_set_blob((nvs_handle_t)nvs_handle_, kNvsKey, &p, sizeof(p)) != ESP_OK) {
        ESP_LOGE(TAG, "failed to save reminders");
        return false;
    }
    if (nvs_commit((nvs_handle_t)nvs_handle_) != ESP_OK) {
        ESP_LOGE(TAG, "failed to commit reminders");
        return false;
    }
    return true;
}

ReminderManager::Result ReminderManager::Add(const std::string& message, int hour,
                                             int minute, int day_offset,
                                             int delay_seconds, uint32_t* out_id,
                                             int64_t* out_due)
{
    if (message.empty() || message.size() >= 64) {
        return kBadArgs;
    }
    if (!TankTimeIsSynced()) {
        return kTimeNotReady;
    }

    const time_t now = time(nullptr);
    int64_t due = 0;

    if (delay_seconds > 0) {
        if (delay_seconds > 7 * 24 * 3600) {
            return kBadArgs;
        }
        due = (int64_t)now + delay_seconds;
    } else {
        if (hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
            day_offset < 0 || day_offset > 6) {
            return kBadArgs;
        }
        struct tm local = {};
        if (!TankTimeLocal(&local)) {
            return kTimeNotReady;
        }
        local.tm_hour = hour;
        local.tm_min = minute;
        local.tm_sec = 0;
        local.tm_mday += day_offset;
        local.tm_isdst = -1;
        time_t target = mktime(&local);
        if (target < 0) {
            return kBadArgs;
        }
        due = (int64_t)target;
        if (day_offset == 0 && due <= (int64_t)now + 5) {
            return kTimePassed;
        }
    }

    xSemaphoreTake((SemaphoreHandle_t)lock_, portMAX_DELAY);
    if (count_ >= kMaxReminders) {
        xSemaphoreGive((SemaphoreHandle_t)lock_);
        return kFull;
    }

    uint32_t id = next_id_ == 0 ? 1 : next_id_;
    bool used = true;
    for (int tries = 0; tries < 16 && used; ++tries) {
        used = false;
        for (size_t i = 0; i < count_; ++i) {
            if (items_[i].id == id) {
                used = true;
                break;
            }
        }
        if (used) {
            id = id == UINT32_MAX ? 1 : id + 1;
        }
    }
    next_id_ = id == UINT32_MAX ? 1 : id + 1;

    Record& r = items_[count_++];
    r.id = id;
    r.due_epoch = due;
    std::strncpy(r.message, message.c_str(), sizeof(r.message) - 1);
    r.message[sizeof(r.message) - 1] = '\0';
    SaveLocked();
    xSemaphoreGive((SemaphoreHandle_t)lock_);

    if (out_id != nullptr) *out_id = id;
    if (out_due != nullptr) *out_due = due;
    ESP_LOGI(TAG, "added #%u due=%lld msg=%s", (unsigned)id,
             (long long)due, message.c_str());
    return kOk;
}

ReminderManager::Result ReminderManager::Delete(uint32_t id)
{
    xSemaphoreTake((SemaphoreHandle_t)lock_, portMAX_DELAY);
    for (size_t i = 0; i < count_; ++i) {
        if (items_[i].id == id) {
            for (size_t j = i; j + 1 < count_; ++j) {
                items_[j] = items_[j + 1];
            }
            count_--;
            SaveLocked();
            xSemaphoreGive((SemaphoreHandle_t)lock_);
            ESP_LOGI(TAG, "deleted #%u", (unsigned)id);
            return kOk;
        }
    }
    xSemaphoreGive((SemaphoreHandle_t)lock_);
    return kNotFound;
}

ReminderManager::Result ReminderManager::Clear()
{
    xSemaphoreTake((SemaphoreHandle_t)lock_, portMAX_DELAY);
    count_ = 0;
    SaveLocked();
    xSemaphoreGive((SemaphoreHandle_t)lock_);
    ESP_LOGI(TAG, "cleared");
    return kOk;
}

size_t ReminderManager::Snapshot(ReminderView* out, size_t max) const
{
    if (out == nullptr || max == 0) {
        return 0;
    }
    xSemaphoreTake((SemaphoreHandle_t)lock_, portMAX_DELAY);
    size_t n = count_ < max ? count_ : max;
    for (size_t i = 0; i < n; ++i) {
        out[i].id = items_[i].id;
        out[i].due_epoch = items_[i].due_epoch;
        std::strncpy(out[i].message, items_[i].message, sizeof(out[i].message) - 1);
        out[i].message[sizeof(out[i].message) - 1] = '\0';
    }
    xSemaphoreGive((SemaphoreHandle_t)lock_);
    return n;
}

void ReminderManager::ManagerTask(void* arg)
{
    auto* self = static_cast<ReminderManager*>(arg);
    while (true) {
        if (!TankTimeIsSynced()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        Notice notices[kMaxReminders];
        size_t notice_count = 0;
        bool changed = false;
        const int64_t now = (int64_t)time(nullptr);

        xSemaphoreTake((SemaphoreHandle_t)self->lock_, portMAX_DELAY);
        for (size_t i = 0; i < self->count_;) {
            Record& r = self->items_[i];
            if (r.due_epoch > now) {
                ++i;
                continue;
            }

            const int64_t late = now - r.due_epoch;
            if (late <= 24 * 3600) {
                Notice& n = notices[notice_count++];
                const char* prefix = late > 60 ? "错过的提醒：" : "提醒：";
                snprintf(n.text, sizeof(n.text), "%s%s", prefix, r.message);
            } else {
                ESP_LOGW(TAG, "dropping stale reminder #%u (%lld s late)",
                         (unsigned)r.id, (long long)late);
            }
            ESP_LOGI(TAG, "due #%u msg=%s", (unsigned)r.id, r.message);

            for (size_t j = i; j + 1 < self->count_; ++j) {
                self->items_[j] = self->items_[j + 1];
            }
            self->count_--;
            changed = true;
        }
        if (changed) {
            self->SaveLocked();
        }
        xSemaphoreGive((SemaphoreHandle_t)self->lock_);

        for (size_t i = 0; i < notice_count; ++i) {
            if (xQueueSend((QueueHandle_t)self->notify_queue_, &notices[i], 0) != pdTRUE) {
                ESP_LOGW(TAG, "notification queue full; dropping %s", notices[i].text);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void ReminderManager::NotifyTask(void* arg)
{
    auto* self = static_cast<ReminderManager*>(arg);
    Notice notice;
    while (true) {
        if (xQueueReceive((QueueHandle_t)self->notify_queue_, &notice, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Do not interrupt an active conversation or OTA. Wait for idle.
        while (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        Application::GetInstance().Schedule([text = std::string(notice.text)]() {
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetChatMessage("assistant", text.c_str());
            }
            Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
        });

        // Space out multiple reminders so they do not overwrite each other.
        vTaskDelay(pdMS_TO_TICKS(8000));
    }
}
