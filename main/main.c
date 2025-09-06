#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "bsp/esp32_p4_nano.h"
#include "lvgl.h"

static const char *TAG = "ESP32_P4_DSI";

// Ethernet networking
#define ETH_CONNECTED_BIT BIT0
static EventGroupHandle_t s_network_event_group;
static httpd_handle_t server = NULL;
static char device_ip[16] = {0};
static char device_mac[18] = {0};

// Image display structures
typedef struct {
    uint8_t *data;
    size_t size;
    uint16_t width;
    uint16_t height;
    bool is_config;
} frame_message_t;

// Frame handling
static QueueHandle_t frame_queue = NULL;
static uint16_t current_frame_width = 0;
static uint16_t current_frame_height = 0;
static bool config_received = false;
static lv_display_t *display_handle = NULL;

// Common frame dimension detection
typedef struct {
    uint16_t width;
    uint16_t height;
    uint32_t bytes;
} frame_dimension_t;

static const frame_dimension_t common_dimensions[] = {
    {240, 240, 115200},     // 240x240 = 115,200 bytes
    {320, 240, 153600},     // 320x240 = 153,600 bytes  
    {480, 320, 307200},     // 480x320 = 307,200 bytes
    {640, 480, 614400},     // 640x480 = 614,400 bytes
    {800, 600, 960000},     // 800x600 = 960,000 bytes
    {1024, 768, 1572864},   // 1024x768 = 1,572,864 bytes
    {1280, 720, 1843200},   // 1280x720 = 1,843,200 bytes
    {0, 0, 0}               // Terminator
};

static bool detect_frame_dimensions(uint32_t frame_size, uint16_t *width, uint16_t *height)
{
    // Check common dimensions first
    for (int i = 0; common_dimensions[i].bytes > 0; i++) {
        if (common_dimensions[i].bytes == frame_size) {
            *width = common_dimensions[i].width;
            *height = common_dimensions[i].height;
            ESP_LOGI(TAG, "Detected common dimension: %dx%d (%d bytes)", *width, *height, frame_size);
            return true;
        }
    }
    
    // Try to find square-ish dimensions
    uint32_t pixels = frame_size / 2;
    uint32_t sqrt_pixels = (uint32_t)sqrt((double)pixels);
    
    // Search around the square root for factors
    for (uint32_t w = sqrt_pixels; w > 0 && w > sqrt_pixels - 100; w--) {
        if (pixels % w == 0) {
            uint32_t h = pixels / w;
            // Check if dimensions are reasonable (not too extreme ratio)
            if (h <= 2048 && w <= 2048) {  // Reasonable limits
                *width = (uint16_t)w;
                *height = (uint16_t)h;
                ESP_LOGI(TAG, "Auto-detected dimension: %dx%d (%d bytes)", *width, *height, frame_size);
                return true;
            }
        }
    }
    
    ESP_LOGW(TAG, "Could not detect dimensions for %d bytes", frame_size);
    return false;
}

static void rgb565_to_lvgl_display(const uint8_t *rgb565_data, uint32_t data_size, uint16_t width, uint16_t height)
{
    if (!display_handle) {
        ESP_LOGE(TAG, "Display handle not initialized");
        return;
    }

    uint32_t expected_bytes = (uint32_t)width * (uint32_t)height * 2U;
    if (data_size != expected_bytes) {
        ESP_LOGW(TAG, "Frame size mismatch: got %u bytes, expected %u for %dx%d",
                 (unsigned) data_size, (unsigned) expected_bytes, width, height);
    }

    int32_t disp_width = lv_display_get_horizontal_resolution(display_handle);
    int32_t disp_height = lv_display_get_vertical_resolution(display_handle);

    ESP_LOGI(TAG, "Updating display with %dx%d frame (display is %dx%d)",
             width, height, disp_width, disp_height);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);

    lv_obj_t *img_obj = lv_img_create(screen);

    float scale_x = (float)disp_width / (float)width;
    float scale_y = (float)disp_height / (float)height;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale > 1.0f) scale = 1.0f;

    int32_t scaled_width = (int32_t)(width * scale);
    int32_t scaled_height = (int32_t)(height * scale);
    int32_t pos_x = (disp_width - scaled_width) / 2;
    int32_t pos_y = (disp_height - scaled_height) / 2;

    /* Build an lv_img_dsc_t dynamically and set header fields according to
       the LVGL version: modern LVGLs provide LV_COLOR_FORMAT_RGB565 (or similar).
       If that symbol isn't available in your LVGL, try LV_IMG_CF_TRUE_COLOR (older API). */

    static lv_img_dsc_t img_dsc;

    // Zero the structure first
    memset(&img_dsc, 0, sizeof(img_dsc));

    // Set header fields — set only the fields that exist in most LVGLs:
    img_dsc.header.w = width;
    img_dsc.header.h = height;

    // Try to set a RGB565-specific constant if available.
    // Preferred modern constant:
    #ifdef LV_COLOR_FORMAT_RGB565
        img_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    #elif defined(LV_COLOR_FORMAT_NATIVE) && defined(LV_COLOR_DEPTH) && (LV_COLOR_DEPTH == 16)
        // If LV_COLOR_FORMAT_NATIVE exists and color depth is 16, native usually maps to RGB565
        img_dsc.header.cf = LV_COLOR_FORMAT_NATIVE;
    #elif defined(LV_IMG_CF_TRUE_COLOR)
        // Backwards-compatible fallback for older LVGL versions (v7/v8)
        img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    #else
        // As an ultimate fallback, try a numeric value that works with many LVGL configs:
        //  -> In some LVGL builds the numeric value for RGB565 is 2 (but this is not portable).
        // So we leave it zero and hope LVGL is built for RGB565; otherwise adjust per your LVGL.
        img_dsc.header.cf = 0;
    #endif

    img_dsc.data_size = data_size;
    img_dsc.data = (const uint8_t *)rgb565_data;

    // Use variable image source
    lv_img_set_src(img_obj, &img_dsc);

    // Position and scale the image
    lv_obj_set_pos(img_obj, pos_x, pos_y);
    lv_obj_set_size(img_obj, scaled_width, scaled_height);

    // Force refresh
    lv_refr_now(display_handle);

    ESP_LOGI(TAG, "Frame displayed: %dx%d scaled to %dx%d at (%d,%d)",
             width, height, scaled_width, scaled_height, pos_x, pos_y);
}


static void frame_display_task(void *pvParameters)
{
    frame_message_t frame_msg;
    
    ESP_LOGI(TAG, "Frame display task started");
    
    while (1) {
        // Wait for frame data
        if (xQueueReceive(frame_queue, &frame_msg, portMAX_DELAY) == pdTRUE) {
            if (frame_msg.is_config) {
                // Handle configuration message
                ESP_LOGI(TAG, "Config updated: %dx%d", frame_msg.width, frame_msg.height);
                current_frame_width = frame_msg.width;
                current_frame_height = frame_msg.height;
                config_received = true;
            } else if (frame_msg.data && frame_msg.size > 0) {
                // Handle frame data
                uint16_t display_width = frame_msg.width;
                uint16_t display_height = frame_msg.height;
                
                // Show frame info
                ESP_LOGI(TAG, "Processing frame: %d bytes", frame_msg.size);
                
                // Display frame
                rgb565_to_lvgl_display(frame_msg.data, frame_msg.size, display_width, display_height);
            }
            
            // Free the frame data
            if (frame_msg.data) {
                free(frame_msg.data);
            }
        }
        
        // Reset watchdog
        esp_task_wdt_reset();
    }
}

// Ethernet event handlers (unchanged)
static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(*(esp_eth_handle_t*)event_data, ETH_CMD_G_MAC_ADDR, mac_addr);
        snprintf(device_mac, sizeof(device_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        ESP_LOGI(TAG, "Ethernet Link Up - MAC: %s", device_mac);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    snprintf(device_ip, sizeof(device_ip), IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Got IP Address: %s", device_ip);
    xEventGroupSetBits(s_network_event_group, ETH_CONNECTED_BIT);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    static uint32_t message_count = 1;
    
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket client connected");
        
        // Send device-connected message (like the Python script)
        cJSON *device_info = cJSON_CreateObject();
        cJSON_AddStringToObject(device_info, "type", "device-connected");
        cJSON_AddNumberToObject(device_info, "timestamp", esp_timer_get_time() / 1000);
        cJSON_AddStringToObject(device_info, "mac", device_mac);
        cJSON_AddStringToObject(device_info, "ip", device_ip);
        cJSON_AddNumberToObject(device_info, "port", 81);
        cJSON_AddStringToObject(device_info, "protocol", "WebSocket");
        cJSON_AddNumberToObject(device_info, "clientId", 1);
        cJSON_AddStringToObject(device_info, "note", "ESP32-P4-Nano Ready for blit frames (dynamic dimensions)");
        
        char *json_string = cJSON_Print(device_info);
        if (json_string) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_string, strlen(json_string));
            free(json_string);
        }
        cJSON_Delete(device_info);
        return ESP_OK;
    }
    
    // Handle incoming data
    char *buf = malloc(64 * 1024);  // 64KB buffer for larger frames
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate receive buffer");
        return ESP_ERR_NO_MEM;
    }
    
    int recv_len = httpd_req_recv(req, buf, 64 * 1024 - 1);
    if (recv_len > 0) {
        // Check if it's text or binary
        bool is_binary = false;
        
        // Simple heuristic: if first few bytes look like binary data, treat as binary
        for (int i = 0; i < MIN(recv_len, 16); i++) {
            if (buf[i] < 32 && buf[i] != '\n' && buf[i] != '\r' && buf[i] != '\t') {
                is_binary = true;
                break;
            }
        }
        
        if (is_binary) {
            // Binary frame data
            ESP_LOGI(TAG, "#%d: FRAME - %d bytes", message_count, recv_len);
            
            // Show first few bytes as hex
            ESP_LOGI(TAG, "First 8 bytes: %02x %02x %02x %02x %02x %02x %02x %02x", 
                     (uint8_t)buf[0], (uint8_t)buf[1], (uint8_t)buf[2], (uint8_t)buf[3],
                     (uint8_t)buf[4], (uint8_t)buf[5], (uint8_t)buf[6], (uint8_t)buf[7]);
            
            // Determine frame dimensions
            uint16_t display_width = current_frame_width;
            uint16_t display_height = current_frame_height;
            
            if (config_received && current_frame_width && current_frame_height) {
                // Use config dimensions
                uint32_t expected_size = current_frame_width * current_frame_height * 2;
                if (recv_len == expected_size) {
                    ESP_LOGI(TAG, "RGB565 Frame (from config): %dx%d pixels (%d bytes)", 
                             current_frame_width, current_frame_height, recv_len);
                } else {
                    ESP_LOGW(TAG, "Frame size mismatch with config: got %d bytes, expected %d for %dx%d", 
                             recv_len, expected_size, current_frame_width, current_frame_height);
                    // Try to detect actual dimensions
                    if (!detect_frame_dimensions(recv_len, &display_width, &display_height)) {
                        ESP_LOGW(TAG, "Could not detect dimensions, using config anyway");
                    }
                }
            } else {
                // Try to auto-detect dimensions
                if (detect_frame_dimensions(recv_len, &display_width, &display_height)) {
                    ESP_LOGI(TAG, "Auto-detected RGB565 Frame: %dx%d pixels (%d bytes)", 
                             display_width, display_height, recv_len);
                } else {
                    ESP_LOGW(TAG, "Unknown frame format: %d bytes (could not detect dimensions)", recv_len);
                    // Use a reasonable fallback
                    display_width = 640;
                    display_height = 480;
                }
            }
            
            // Queue frame for display
            if (frame_queue && display_width && display_height) {
                frame_message_t frame_msg = {0};
                frame_msg.data = malloc(recv_len);
                if (frame_msg.data) {
                    memcpy(frame_msg.data, buf, recv_len);
                    frame_msg.size = recv_len;
                    frame_msg.width = display_width;
                    frame_msg.height = display_height;
                    frame_msg.is_config = false;
                    
                    if (xQueueSend(frame_queue, &frame_msg, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "Frame queue full, dropping frame");
                        free(frame_msg.data);
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to allocate frame data");
                }
            }
            
        } else {
            // Text message (likely config or JSON)
            buf[recv_len] = '\0';
            ESP_LOGI(TAG, "#%d: TEXT - %d chars", message_count, recv_len);
            
            // Handle ping/pong
            if (strncmp(buf, "ping", 4) == 0) {
                httpd_resp_send(req, "pong", 4);
                ESP_LOGI(TAG, "Ping/Pong");
                free(buf);
                return ESP_OK;
            }
            
            // Try to parse as JSON
            cJSON *json = cJSON_Parse(buf);
            if (json) {
                cJSON *type_item = cJSON_GetObjectItem(json, "type");
                const char *msg_type = type_item ? cJSON_GetStringValue(type_item) : "unknown";
                
                if (strcmp(msg_type, "blit_config") == 0) {
                    ESP_LOGI(TAG, "BLIT CONFIG received:");
                    
                    cJSON *mode = cJSON_GetObjectItem(json, "mode");
                    cJSON *format = cJSON_GetObjectItem(json, "frameFormat");
                    cJSON *width = cJSON_GetObjectItem(json, "frameWidth");
                    cJSON *height = cJSON_GetObjectItem(json, "frameHeight");
                    cJSON *size = cJSON_GetObjectItem(json, "frameSize");
                    cJSON *desc = cJSON_GetObjectItem(json, "description");
                    
                    if (mode) ESP_LOGI(TAG, "- Mode: %s", cJSON_GetStringValue(mode));
                    if (format) ESP_LOGI(TAG, "- Format: %s", cJSON_GetStringValue(format));
                    if (width && height) {
                        ESP_LOGI(TAG, "- Dimensions: %dx%d", cJSON_GetNumberValue(width), cJSON_GetNumberValue(height));
                    }
                    if (size) ESP_LOGI(TAG, "- Frame Size: %d bytes", (int)cJSON_GetNumberValue(size));
                    if (desc) ESP_LOGI(TAG, "- Description: %s", cJSON_GetStringValue(desc));
                    
                    // Update frame dimensions from config
                    if (width && height) {
                        frame_message_t config_msg = {0};
                        config_msg.width = (uint16_t)cJSON_GetNumberValue(width);
                        config_msg.height = (uint16_t)cJSON_GetNumberValue(height);
                        config_msg.is_config = true;
                        
                        if (frame_queue) {
                            xQueueSend(frame_queue, &config_msg, 0);
                        }
                    }
                    
                } else if (strcmp(msg_type, "rive_config") == 0 || strcmp(msg_type, "config") == 0) {
                    ESP_LOGI(TAG, "CONFIG: %s", msg_type);
                    cJSON *screen_id = cJSON_GetObjectItem(json, "screenId");
                    if (screen_id) {
                        ESP_LOGI(TAG, "- Screen ID: %s", cJSON_GetStringValue(screen_id));
                    }
                    
                } else if (strcmp(msg_type, "rive_sensor") == 0 || strcmp(msg_type, "sensor") == 0) {
                    ESP_LOGI(TAG, "SENSOR DATA: %s", msg_type);
                    cJSON *sensors = cJSON_GetObjectItem(json, "sensors");
                    if (sensors) {
                        cJSON *sensor = NULL;
                        cJSON_ArrayForEach(sensor, sensors) {
                            const char *sensor_name = sensor->string;
                            cJSON *display_value = cJSON_GetObjectItem(sensor, "displayValue");
                            cJSON *value = cJSON_GetObjectItem(sensor, "value");
                            
                            if (display_value) {
                                ESP_LOGI(TAG, "- %s = %s", sensor_name, cJSON_GetStringValue(display_value));
                            } else if (value) {
                                ESP_LOGI(TAG, "- %s = %.2f", sensor_name, cJSON_GetNumberValue(value));
                            }
                        }
                    }
                    
                } else {
                    ESP_LOGI(TAG, "JSON: %s", msg_type);
                    ESP_LOGI(TAG, "- Content: %.100s...", buf);
                }
                
                cJSON_Delete(json);
            } else {
                // Not JSON, show as plain text
                ESP_LOGI(TAG, "PLAIN TEXT: %.100s...", buf);
            }
        }
        
        message_count++;
    }
    
    free(buf);
    return ESP_OK;
}

static void start_ethernet_and_webserver(void *pvParameters)
{
    // Initialize networking (unchanged from original)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    // ESP32-P4-Nano specific Ethernet configuration (unchanged)
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_gpio.mdc_num = 31;    
    esp32_emac_config.smi_gpio.mdio_num = 52;   
    esp32_emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    esp32_emac_config.clock_config.rmii.clock_gpio = 50; 

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;          
    phy_config.reset_gpio_num = 51;   

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config); 
    
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    
    // Wait for IP
    EventBits_t bits = xEventGroupWaitBits(s_network_event_group, ETH_CONNECTED_BIT, 
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    
    if (bits & ETH_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WebSocket Server starting at ws://%s:81/", device_ip);
        
        httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
        httpd_config.server_port = 81;
        httpd_config.max_uri_handlers = 16;
        httpd_config.max_resp_headers = 16;
        
        if (httpd_start(&server, &httpd_config) == ESP_OK) {
            httpd_uri_t ws = {
                .uri = "/",
                .method = HTTP_GET,
                .handler = ws_handler,
                .user_ctx = NULL
            };
            httpd_register_uri_handler(server, &ws);
            
            httpd_uri_t ws_post = {
                .uri = "/",
                .method = HTTP_POST,
                .handler = ws_handler,
                .user_ctx = NULL
            };
            httpd_register_uri_handler(server, &ws_post);
            
            ESP_LOGI(TAG, "WebSocket server ready for blit frames!");
        }
    } else {
        ESP_LOGW(TAG, "No Ethernet connection, continuing without WebSocket");
    }
    
    // Keep task alive
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (server != NULL) {
            ESP_LOGD(TAG, "WebSocket server running on %s:81", device_ip);
        }
    }
    
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-P4-Nano WebSocket Image Display Ready");

    // Create frame queue for passing data between WebSocket and display tasks
    frame_queue = xQueueCreate(5, sizeof(frame_message_t));
    if (!frame_queue) {
        ESP_LOGE(TAG, "Failed to create frame queue");
        return;
    }

    // Initialize networking first (non-blocking)
    s_network_event_group = xEventGroupCreate();
    xTaskCreate(start_ethernet_and_webserver, "network_task", 8192, NULL, 5, NULL);

    // Initialize display using BSP
    display_handle = bsp_display_start();
    if (display_handle == NULL) {
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

    // Create frame display task
    xTaskCreate(frame_display_task, "frame_display", 8192, NULL, 4, NULL);

    // Register main task with watchdog
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());

    // Show ready message on display
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_make(0, 0, 0), 0);  // Black background
    
    lv_obj_t *label = lv_label_create(screen);
    lv_label_set_text(label, "ESP32-P4-Nano\nReady for WebSocket\nBlit Frames\n\nWaiting for connection...");
    lv_obj_set_style_text_color(label, lv_color_make(255, 255, 255), 0);  // White text
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    
    lv_refr_now(display_handle);

    ESP_LOGI(TAG, "Ready to receive and display RGB565 frames via WebSocket");
    ESP_LOGI(TAG, "Connect JunctionRelay to ws://%s:81/", device_ip[0] ? device_ip : "[waiting for IP]");

    // Main loop - just keep watchdog happy
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_task_wdt_reset();
    }
}