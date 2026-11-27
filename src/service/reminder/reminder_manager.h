#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct ReminderView {
    uint32_t id;
    int64_t due_epoch;
    char message[64];
};

class ReminderManager {
public:
    enum Result {
        kOk = 0,
        kTimeNotReady = -1,
        kFull = -2,
        kTimePassed = -3,
        kNotFound = -4,
        kBadArgs = -5,
        kNvsError = -6,
    };

    static ReminderManager& GetInstance();

    void Init();

    // delay_seconds > 0 takes precedence over hour/minute/day_offset.
    Result Add(const std::string& message, int hour, int minute, int day_offset,
               int delay_seconds, uint32_t* out_id = nullptr,
               int64_t* out_due = nullptr);

    Result Delete(uint32_t id);
    Result Clear();
    size_t Snapshot(ReminderView* out, size_t max) const;

private:
    ReminderManager() = default;
    ~ReminderManager() = default;
    ReminderManager(const ReminderManager&) = delete;
    ReminderManager& operator=(const ReminderManager&) = delete;

    struct Record {
        uint32_t id;
        int64_t due_epoch;
        char message[64];
    };

    struct Persist {
        uint32_t magic;
        uint32_t count;
        uint32_t next_id;
        Record items[8];
    };

    struct Notice {
        char text[96];
    };

    static constexpr size_t kMaxReminders = 8;
    static constexpr uint32_t kMagic = 0x52454D31;  // "REM1"

    static void ManagerTask(void* arg);
    static void NotifyTask(void* arg);

    void LoadLocked();
    bool SaveLocked();

    mutable void* lock_ = nullptr;
    void* notify_queue_ = nullptr;
    uint32_t nvs_handle_ = 0;
    Record items_[kMaxReminders] = {};
    size_t count_ = 0;
    uint32_t next_id_ = 1;
    bool initialized_ = false;
    bool nvs_ok_ = false;
};
