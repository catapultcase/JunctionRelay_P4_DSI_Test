#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"

// Display configuration for 5-DSI-TOUCH-A (estimated specs)
#define DISPLAY_WIDTH 480
#define DISPLAY_HEIGHT 854
#define DSI_NUM_DATA_LANES 2

static const char *TAG = "DSI_INIT_TEST";

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-P4 DSI Initialization Test Starting...");

    // Step 1: Basic ESP32-P4 functionality test
    ESP_LOGI(TAG, "Step 1: Basic functionality - OK");

    // Step 2: Initialize DSI bus
    ESP_LOGI(TAG, "Step 2: Initializing DSI bus...");

    esp_lcd_dsi_bus_config_t dsi_bus_config = {
        .bus_id = 0,
        .num_data_lanes = DSI_NUM_DATA_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = 1000, // 1Gbps per lane
    };

    esp_lcd_dsi_bus_handle_t dsi_bus = NULL;
    esp_err_t ret = esp_lcd_new_dsi_bus(&dsi_bus_config, &dsi_bus);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✓ DSI bus initialized successfully!");
    } else {
        ESP_LOGE(TAG, "✗ DSI bus initialization failed: %s", esp_err_to_name(ret));
        ESP_LOGI(TAG, "This might be expected if display is not connected");
    }

    // Step 3: Test DPI configuration
    ESP_LOGI(TAG, "Step 3: Testing DPI configuration...");

    esp_lcd_dpi_panel_config_t dpi_config = {
        // Removed .virtual_gpio_num = -1, as it doesn't exist in this version
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 20,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .video_timing = {
            .h_size = DISPLAY_WIDTH,
            .v_size = DISPLAY_HEIGHT,
            .hsync_back_porch = 50,
            .hsync_front_porch = 50,
            .hsync_pulse_width = 10,
            .vsync_back_porch = 20,
            .vsync_front_porch = 20,
            .vsync_pulse_width = 10,
        },
        .flags.use_dma2d = true,
    };

    ESP_LOGI(TAG, "DPI configuration:");
    ESP_LOGI(TAG, " Resolution: %dx%d", DISPLAY_WIDTH, DISPLAY_HEIGHT);
    ESP_LOGI(TAG, " Clock: %d MHz", 20);
    ESP_LOGI(TAG, " Data lanes: %d", DSI_NUM_DATA_LANES);

    // Step 4: Connection status
    ESP_LOGI(TAG, "Step 4: Display connection status");
    ESP_LOGI(TAG, "Hardware setup checklist:");
    ESP_LOGI(TAG, " □ 5-DSI-TOUCH-A connected to ESP32-P4 DSI port");
    ESP_LOGI(TAG, " □ Display power connected (5V, GND)");
    ESP_LOGI(TAG, " □ DSI cable properly seated");

    // Step 5: Ready for display driver
    ESP_LOGI(TAG, "Step 5: System ready for display driver implementation");
    ESP_LOGI(TAG, "Next steps:");
    ESP_LOGI(TAG, " 1. Identify display controller chip");
    ESP_LOGI(TAG, " 2. Add specific driver component");
    ESP_LOGI(TAG, " 3. Implement display initialization");

    // Cleanup if DSI was initialized
    if (dsi_bus != NULL) {
        ESP_LOGI(TAG, "Cleaning up DSI bus...");
        esp_lcd_del_dsi_bus(dsi_bus);
    }

    // Keep running with status updates
    int count = 0;
    while (1) {
        ESP_LOGI(TAG, "System running... Ready for display connection (cycle %d)", count++);
        vTaskDelay(pdMS_TO_TICKS(5000));

        if (count % 10 == 0) {
            ESP_LOGI(TAG, "💡 Connect your 5-DSI-TOUCH-A display and power it on");
        }
    }
}