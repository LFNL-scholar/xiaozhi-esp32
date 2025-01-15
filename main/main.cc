#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>

#include "application.h"
#include "system_info.h"

#define TAG "main"

extern "C" void app_main(void)
{
    // Initialize the default event loop | 初始化默认事件循环
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration | 初始化NVS（非易失性存储）用于存储配置数据（如Wi-Fi设置）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Launch the application | 启动应用程序
    Application::GetInstance().Start();

    // Dump CPU usage every 10 second | 循环监控内存状态
    while (true) {
        vTaskDelay(10000 / portTICK_PERIOD_MS); // 延迟10秒
        // SystemInfo::PrintRealTimeStats(pdMS_TO_TICKS(1000));
        int free_sram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL); // 当前可用的内部堆内存大小
        int min_free_sram = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL); // 系统启动以来的最小可用堆内存大小
        ESP_LOGI(TAG, "Free internal: %u minimal internal: %u", free_sram, min_free_sram);
    }
}
