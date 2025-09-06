#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "bsp/esp32_p4_nano.h"

static const char *TAG = "ESP32_P4_DSI";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-P4-Nano + 5-DSI-TOUCH-A Test");

    // Initialize display using BSP
    lv_display_t *display = bsp_display_start();
    
    if (display == NULL) {
        ESP_LOGE(TAG, "Display initialization failed");
        return;
    }
    ESP_LOGI(TAG, "Display initialized successfully!");

    // Initialize backlight
    esp_err_t ret = bsp_display_brightness_init();
    if (ret == ESP_OK) {
        bsp_display_backlight_on();
        bsp_display_brightness_set(80);
        ESP_LOGI(TAG, "Backlight on at 80%%");
    }

    ESP_LOGI(TAG, "Display test complete - screen should be lit");

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "System running...");
    }
}