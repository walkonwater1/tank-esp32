#include "reminder_manager.h"

#include <cstring>
#include <ctime>

#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "esp_log.h"
#include "esp_tts.h"
#include "esp_tts_voice_xiaole.h"
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
    xTaskCreate(NotifyTask, "reminder_notify", 16384, this, 2, nullptr);
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
    for (size_t i = 0; i < count_; ++i) {
        ESP_LOGI(TAG, "loaded #%u due=%u msg=%s", (unsigned)items_[i].id,
                 (unsigned)items_[i].due_epoch, items_[i].message);
    }
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

bool ReminderManager::IsPendingLocked(uint32_t id) const
{
    for (size_t i = 0; i < pending_count_; ++i) {
        if (pending_ids_[i] == id) {
            return true;
        }
    }
    return false;
}

bool ReminderManager::MarkPendingLocked(uint32_t id)
{
    if (IsPendingLocked(id) || pending_count_ >= kMaxReminders) {
        return false;
    }
    pending_ids_[pending_count_++] = id;
    return true;
}

void ReminderManager::ClearPendingLocked(uint32_t id)
{
    for (size_t i = 0; i < pending_count_; ++i) {
        if (pending_ids_[i] == id) {
            for (size_t j = i; j + 1 < pending_count_; ++j) {
                pending_ids_[j] = pending_ids_[j + 1];
            }
            pending_count_--;
            return;
        }
    }
}

void ReminderManager::Complete(uint32_t id)
{
    xSemaphoreTake((SemaphoreHandle_t)lock_, portMAX_DELAY);
    ClearPendingLocked(id);
    for (size_t i = 0; i < count_; ++i) {
        if (items_[i].id == id) {
            for (size_t j = i; j + 1 < count_; ++j) {
                items_[j] = items_[j + 1];
            }
            count_--;
            SaveLocked();
            xSemaphoreGive((SemaphoreHandle_t)lock_);
            ESP_LOGI(TAG, "completed #%u", (unsigned)id);
            return;
        }
    }
    xSemaphoreGive((SemaphoreHandle_t)lock_);
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
    ESP_LOGI(TAG, "added #%u due=%u msg=%s", (unsigned)id,
             (unsigned)due, message.c_str());
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
            ClearPendingLocked(id);
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
    pending_count_ = 0;
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
            if (late > 24 * 3600) {
                ESP_LOGW(TAG, "dropping stale reminder #%u (%d s late)",
                         (unsigned)r.id, (int)late);
                self->ClearPendingLocked(r.id);
                for (size_t j = i; j + 1 < self->count_; ++j) {
                    self->items_[j] = self->items_[j + 1];
                }
                self->count_--;
                changed = true;
                continue;
            }

            // Keep the record until NotifyTask has actually delivered it.
            // Mark it pending so the next poll does not enqueue it again.
            if (self->IsPendingLocked(r.id)) {
                ++i;
                continue;
            }
            if (!self->MarkPendingLocked(r.id)) {
                ESP_LOGW(TAG, "pending queue full; reminder #%u stays queued",
                         (unsigned)r.id);
                ++i;
                continue;
            }

            Notice& n = notices[notice_count++];
            n.id = r.id;
            // Reminders that were missed by more than five minutes only get a
            // popup: this also breaks a crash loop caused by an old reminder
            // firing again on every boot.
            n.allow_speech = late <= 300;
            const char* prefix = late > 60 ? "错过的提醒：" : "提醒：";
            snprintf(n.text, sizeof(n.text), "%s%s", prefix, r.message);
            ESP_LOGI(TAG, "due #%u msg=%s late=%d speech=%d", (unsigned)r.id,
                     r.message, (int)late, n.allow_speech ? 1 : 0);
            ++i;
        }
        if (changed) {
            self->SaveLocked();
        }
        xSemaphoreGive((SemaphoreHandle_t)self->lock_);

        for (size_t i = 0; i < notice_count; ++i) {
            if (xQueueSend((QueueHandle_t)self->notify_queue_, &notices[i], 0) != pdTRUE) {
                ESP_LOGW(TAG, "notification queue full; will retry %s", notices[i].text);
                xSemaphoreTake((SemaphoreHandle_t)self->lock_, portMAX_DELAY);
                self->ClearPendingLocked(notices[i].id);
                xSemaphoreGive((SemaphoreHandle_t)self->lock_);
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

        auto& app = Application::GetInstance();
        ESP_LOGI(TAG, "notify #%u state=%d text=%s", (unsigned)notice.id,
                 (int)app.GetDeviceState(), notice.text);

        // Give an active conversation a short window to finish, but never
        // wait forever: a stale "listening" state used to swallow reminders.
        int waited_ms = 0;
        while (app.GetDeviceState() != kDeviceStateIdle && waited_ms < 8000) {
            vTaskDelay(pdMS_TO_TICKS(500));
            waited_ms += 500;
        }
        if (app.GetDeviceState() != kDeviceStateIdle) {
            DeviceState state = app.GetDeviceState();
            // StopListening alone leaves the cloud audio channel open, so the
            // assistant keeps sending TTS/ASR and talks over the reminder.
            ESP_LOGW(TAG, "device busy (state=%d) after %d ms; ending conversation",
                     (int)state, waited_ms);
            app.EndConversation();
            for (int i = 0; i < 20 && app.GetDeviceState() != kDeviceStateIdle; ++i) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (app.GetDeviceState() != kDeviceStateIdle) {
                ESP_LOGW(TAG, "device still busy (state=%d); notifying anyway",
                         (int)app.GetDeviceState());
            }
        }

        app.Schedule([text = std::string(notice.text)]() {
            auto display = Board::GetInstance().GetDisplay();
            if (display != nullptr) {
                display->SetChatMessage("assistant", text.c_str());
            }
        });

        // Speak the reminder locally with ESP-SR TTS. If synthesis fails,
        // fall back to the short popup sound.
        bool spoke = false;
        if (!notice.allow_speech) {
            ESP_LOGI(TAG, "notify #%u is too old; popup only", (unsigned)notice.id);
        } else {
            void* voice_data = nullptr;
            size_t voice_size = 0;
            if (!Assets::GetInstance().GetAssetData("esp_tts_voice_data_xiaole.dat",
                                                    voice_data, voice_size)) {
                ESP_LOGW(TAG, "tts #%u voice data missing from assets",
                         (unsigned)notice.id);
            } else {
                ESP_LOGI(TAG, "tts #%u voice data=%u bytes", (unsigned)notice.id,
                         (unsigned)voice_size);
                esp_tts_voice_t* voice =
                    esp_tts_voice_set_init(&esp_tts_voice_xiaole, voice_data);
                if (voice != nullptr) {
                    esp_tts_handle_t tts = esp_tts_create(voice);
                    if (tts != nullptr) {
                        std::string speak_text(notice.text);
                        // Replace the full-width colon with a comma for more natural TTS.
                        size_t pos = 0;
                        const std::string colon = "：";
                        while ((pos = speak_text.find(colon, pos)) != std::string::npos) {
                            speak_text.replace(pos, colon.size(), "，");
                            pos += 3;
                        }
                        if (esp_tts_parse_chinese(tts, speak_text.c_str())) {
                            std::vector<int16_t> pcm;
                            int len = 0;
                            short* chunk = nullptr;
                            while ((chunk = esp_tts_stream_play(tts, &len, 3)) != nullptr && len > 0) {
                                pcm.insert(pcm.end(), chunk, chunk + len);
                            }
                            ESP_LOGI(TAG, "tts #%u pcm=%u samples", (unsigned)notice.id,
                                     (unsigned)pcm.size());
                            if (!pcm.empty()) {
                                app.GetAudioService().PlayPcm(std::move(pcm));
                                spoke = true;
                            }
                        } else {
                            ESP_LOGW(TAG, "tts #%u parse failed", (unsigned)notice.id);
                        }
                        esp_tts_destroy(tts);
                    } else {
                        ESP_LOGW(TAG, "tts #%u create failed", (unsigned)notice.id);
                    }
                    esp_tts_voice_set_free(voice);
                } else {
                    ESP_LOGW(TAG, "tts #%u voice init failed", (unsigned)notice.id);
                }
            }
        }
        if (!spoke) {
            app.PlaySound(Lang::Sounds::OGG_POPUP);
        }
        self->Complete(notice.id);

        // Space out multiple reminders so they do not overwrite each other.
        vTaskDelay(pdMS_TO_TICKS(8000));
    }
}
