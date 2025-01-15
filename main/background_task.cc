#include "background_task.h"

#include <esp_log.h>

#define TAG "BackgroundTask"

// 构造函数，负责创建和初始化一个后台任务
BackgroundTask::BackgroundTask(uint32_t stack_size) {
#if CONFIG_IDF_TARGET_ESP32S3
    // 针对 ESP32-S3 芯片
    // 使用 静态任务分配 和 SPIRAM（外部存储），更适合大内存任务
    task_stack_ = (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
    background_task_handle_ = xTaskCreateStatic([](void* arg) {
        BackgroundTask* task = (BackgroundTask*)arg;
        task->BackgroundTaskLoop();
    }, "background_task", stack_size, this, 1, task_stack_, &task_buffer_);
#else
    // 针对 其他芯片
    // 使用 动态任务分配，任务栈分配在默认内存中
    xTaskCreate([](void* arg) {
        BackgroundTask* task = (BackgroundTask*)arg;
        task->BackgroundTaskLoop();
    }, "background_task", stack_size, this, 1, &background_task_handle_);
#endif
}

// 析构函数，用于释放任务资源，确保内存管理的正确性和系统稳定性
BackgroundTask::~BackgroundTask() {
    // 删除任务
    if (background_task_handle_ != nullptr) {
        vTaskDelete(background_task_handle_);
    }
    // 释放任务栈
    if (task_stack_ != nullptr) {
        heap_caps_free(task_stack_);
    }
}

void BackgroundTask::Schedule(std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_tasks_ >= 30) {
        ESP_LOGW(TAG, "active_tasks_ == %u", active_tasks_.load());
    }
    active_tasks_++;
    main_tasks_.emplace_back([this, cb = std::move(callback)]() {
        cb();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_tasks_--;
            if (main_tasks_.empty() && active_tasks_ == 0) {
                condition_variable_.notify_all();
            }
        }
    });
    condition_variable_.notify_all();
}

void BackgroundTask::WaitForCompletion() {
    std::unique_lock<std::mutex> lock(mutex_);
    condition_variable_.wait(lock, [this]() {
        return main_tasks_.empty() && active_tasks_ == 0;
    });
}

void BackgroundTask::BackgroundTaskLoop() {
    ESP_LOGI(TAG, "background_task started");
    while (true) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_variable_.wait(lock, [this]() { return !main_tasks_.empty(); });
        
        std::list<std::function<void()>> tasks = std::move(main_tasks_);
        lock.unlock();

        for (auto& task : tasks) {
            task();
        }
    }
}
