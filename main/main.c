#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
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

// Ethernet event handlers
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
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket client connected");
        
        cJSON *device_info = cJSON_CreateObject();
        cJSON_AddStringToObject(device_info, "type", "device-connected");
        cJSON_AddNumberToObject(device_info, "timestamp", esp_timer_get_time() / 1000);
        cJSON_AddStringToObject(device_info, "mac", device_mac);
        cJSON_AddStringToObject(device_info, "ip", device_ip);
        cJSON_AddNumberToObject(device_info, "port", 81);
        cJSON_AddStringToObject(device_info, "protocol", "WebSocket");
        cJSON_AddNumberToObject(device_info, "clientId", 1);
        cJSON_AddStringToObject(device_info, "note", "ESP32-P4-Nano Ready for frames");
        
        char *json_string = cJSON_Print(device_info);
        if (json_string) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, json_string, strlen(json_string));
            free(json_string);
        }
        cJSON_Delete(device_info);
        return ESP_OK;
    }
    
    char buf[1024];
    int recv_len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (recv_len > 0) {
        buf[recv_len] = '\0';
        ESP_LOGI(TAG, "WebSocket received: %.*s", recv_len, buf);
        
        if (strncmp(buf, "ping", 4) == 0) {
            httpd_resp_send(req, "pong", 4);
        } else {
            httpd_resp_send(req, buf, recv_len);
        }
    }
    
    return ESP_OK;
}

static void start_ethernet_and_webserver(void *pvParameters)
{
    // Initialize networking
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    // ESP32-P4-Nano specific Ethernet configuration (from SDK Configuration)
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_gpio.mdc_num = 31;    // MDC pin
    esp32_emac_config.smi_gpio.mdio_num = 52;   // MDIO pin
    esp32_emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    esp32_emac_config.clock_config.rmii.clock_gpio = 50;  // REF_CLK input

    // MAC configuration
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    
    // PHY configuration - ESP32-P4-Nano uses IP101 PHY
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;           // PHY address from SDK config
    phy_config.reset_gpio_num = 51;    // PHY reset pin from SDK config

    // Create MAC and PHY instances
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);  // IP101 specific driver
    
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
            
            ESP_LOGI(TAG, "WebSocket server ready!");
        }
    } else {
        ESP_LOGW(TAG, "No Ethernet connection, continuing without WebSocket");
    }
    
    // Task must not return - keep it alive with an infinite loop
    while (1) {
        // You can add periodic network maintenance tasks here if needed
        vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 seconds
        
        // Optional: Log server status periodically
        if (server != NULL) {
            ESP_LOGD(TAG, "WebSocket server running on %s:81", device_ip);
        }
    }
    
    // This should never be reached, but just in case:
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-P4-Nano + DSI Display RGB Test with Ethernet");

    // Initialize networking first (non-blocking)
    s_network_event_group = xEventGroupCreate();
    xTaskCreate(start_ethernet_and_webserver, "network_task", 8192, NULL, 5, NULL);

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

    // Register main task with watchdog to prevent watchdog errors
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());

    // Test colors with names
    struct {
        lv_color_t color;
        const char* name;
    } test_colors[] = {
        {lv_color_make(255, 0, 0), "Red"},
        {lv_color_make(0, 255, 0), "Green"},
        {lv_color_make(0, 0, 255), "Blue"},
        {lv_color_make(255, 255, 0), "Yellow"},
        {lv_color_make(255, 0, 255), "Magenta"},
        {lv_color_make(0, 255, 255), "Cyan"},
        {lv_color_make(255, 255, 255), "White"},
        {lv_color_make(0, 0, 0), "Black"},
        {lv_color_make(128, 128, 128), "Gray"},
        {lv_color_make(255, 165, 0), "Orange"},
        {lv_color_make(128, 0, 128), "Purple"},
        {lv_color_make(0, 128, 0), "Dark Green"}
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