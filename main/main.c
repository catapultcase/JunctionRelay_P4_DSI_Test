#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "bsp/esp32_p4_nano.h"
#include "lvgl.h"

static const char *TAG = "ESP32_P4_DSI";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-P4-Nano + 5-DSI-TOUCH-A RGB Test");

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

    // Test colors with names
    struct {
        lv_color_t color;
        const char* name;
    } test_colors[] = {
        {lv_color_make(255, 0, 0),   "Red"},
        {lv_color_make(0, 255, 0),   "Green"},
        {lv_color_make(0, 0, 255),   "Blue"},
        {lv_color_make(255, 255, 0), "Yellow"},
        {lv_color_make(255, 0, 255), "Magenta"},
        {lv_color_make(0, 255, 255), "Cyan"},
        {lv_color_make(255, 255, 255), "White"},
        {lv_color_make(0, 0, 0),     "Black"},
        {lv_color_make(128, 128, 128), "Gray"},
        {lv_color_make(255, 165, 0), "Orange"},
        {lv_color_make(128, 0, 128), "Purple"},
        {lv_color_make(0, 128, 0),   "Dark Green"}
    };

    int num_colors = sizeof(test_colors) / sizeof(test_colors[0]);

    ESP_LOGI(TAG, "Starting RGB color test with %d colors", num_colors);

    while (1) {
        for (int i = 0; i < num_colors; i++) {
            ESP_LOGI(TAG, "Displaying: %s", test_colors[i].name);
            
            // Set the display background color
            lv_obj_set_style_bg_color(lv_screen_active(), test_colors[i].color, 0);
            
            // Force screen refresh
            lv_refr_now(display);
            
            // Wait 2 seconds before next color
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_task_wdt_reset();
        }
        
        // Add a brief pause between cycles
        ESP_LOGI(TAG, "Cycle complete, restarting...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_task_wdt_reset();
    }
}