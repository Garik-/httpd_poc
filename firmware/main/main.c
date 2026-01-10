#include "driver/gpio.h"
#include "err.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define LED_PIN GPIO_NUM_8
#define LED_BLINK_INTERVAL pdMS_TO_TICKS(512)

#define GPIO_OUTPUT_PIN_SEL ((1ULL << LED_PIN))

static const char *TAG = "httpd_poc";

#define CLOSER_IMPLEMENTATION
#include "closer.h"

static closer_handle_t s_closer = NULL;
#define DEFER(fn) CLOSER_DEFER(s_closer, (void *)fn)

static esp_netif_t *s_sta_netif = NULL;
static httpd_handle_t s_server = NULL;

#define ETAG_LEN 24
static char s_etag[ETAG_LEN];

static esp_err_t gpio_init() {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    return gpio_config(&io_conf);
}

static esp_err_t blink_task() {
    for (;;) {
        ESP_RETURN_ON_ERROR(gpio_set_level(LED_PIN, 1), TAG, "failed to turn on LED");
        vTaskDelay(LED_BLINK_INTERVAL);
        ESP_RETURN_ON_ERROR(gpio_set_level(LED_PIN, 0), TAG, "failed to turn off LED");
        vTaskDelay(LED_BLINK_INTERVAL);
    }
    return ESP_OK;
}

static esp_err_t delete_default_wifi_driver_and_handlers() {
    if (unlikely(s_sta_netif == NULL)) {
        return ESP_OK;
    }

    return esp_wifi_clear_default_wifi_driver_and_handlers(s_sta_netif);
}

static void sta_netif_destroy() {
    if (unlikely(s_sta_netif == NULL)) {
        return;
    }

    esp_netif_destroy(s_sta_netif);
    s_sta_netif = NULL;
}

static esp_err_t wifi_init() {
    ESP_LOGI(TAG, "wifi_init");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    DEFER(esp_netif_deinit);

    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "esp_event_loop_create_default failed");
    DEFER(esp_event_loop_delete_default);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
    DEFER(esp_wifi_deinit);

    esp_netif_inherent_config_t esp_netif_config = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    s_sta_netif = esp_netif_create_wifi(WIFI_IF_STA, &esp_netif_config);

    if (unlikely(s_sta_netif == NULL)) {
        ESP_LOGE(TAG, "esp_netif_create_wifi failed");
        return ESP_FAIL;
    }
    DEFER(sta_netif_destroy);

    ESP_RETURN_ON_ERROR(esp_wifi_set_default_wifi_sta_handlers(), TAG, "esp_wifi_set_default_wifi_sta_handlers failed");
    DEFER(delete_default_wifi_driver_and_handlers);

    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "esp_wifi_set_storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "esp_wifi_set_mode failed");

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "esp_wifi_start failed");
    DEFER(esp_wifi_stop);

    int8_t pwr;
    ESP_RETURN_ON_ERROR(esp_wifi_get_max_tx_power(&pwr), TAG, "esp_wifi_get_max_tx_power failed");
    ESP_LOGI(TAG, "WiFi TX power = %.2f dBm, pwr=%d", pwr * 0.25, pwr);

    return ESP_OK;
}

static esp_err_t nvs_init() {
    esp_err_t ret = nvs_flash_init();
    if (unlikely(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs_flash_erase failed");
        ret = nvs_flash_init();
    }

    return ret;
}

static esp_err_t wifi_connect() {
    wifi_config_t wifi_config = {
        .sta =
            {
                .ssid = CONFIG_HTTPD_WIFI_SSID,
                .password = CONFIG_HTTPD_WIFI_PASSWORD,
            },
    };

    ESP_LOGI(TAG, "Connecting to %s...", wifi_config.sta.ssid);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "esp_wifi_set_config failed");

    return esp_wifi_connect();
}

static esp_err_t make_etag(char *etag, size_t etag_len) {
    if (unlikely(!etag || etag_len < 20)) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_app_desc_t *desc = esp_app_get_description();
    if (unlikely(!desc)) {
        return ESP_FAIL;
    }

    int written =
        snprintf(etag, etag_len, "\"%02x%02x%02x%02x%02x%02x%02x%02x\"", desc->app_elf_sha256[0],
                 desc->app_elf_sha256[1], desc->app_elf_sha256[2], desc->app_elf_sha256[3], desc->app_elf_sha256[4],
                 desc->app_elf_sha256[5], desc->app_elf_sha256[6], desc->app_elf_sha256[7]);

    if (unlikely((written < 0 || (size_t)written >= etag_len))) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

//  Handler to redirect incoming GET request for /index.html to /
static esp_err_t index_html_get_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "307 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0); // Response body can be empty
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req) {

    httpd_resp_set_hdr(req, "ETag", s_etag);

    // Проверяем If-None-Match
    char if_none_match[ETAG_LEN];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", if_none_match, sizeof(if_none_match)) == ESP_OK) {
        if (strncmp(if_none_match, s_etag, ETAG_LEN - 1) == 0) {
            httpd_resp_set_status(req, "304 Not Modified");
            return httpd_resp_send(req, NULL, 0);
        }
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, must-revalidate");

    // "no-cache, must-revalidate" - for dynamic content
    // "public, max-age=300, s-maxage=86400, stale-while-revalidate=300, stale-if-error=3600" - for static files behind
    // "public, max-age=31536000, immutable" - for versioned static files

    extern const unsigned char index_html_start[] asm("_binary_index_html_gz_start");
    extern const unsigned char index_html_end[] asm("_binary_index_html_gz_end");
    const size_t index_html_size = (index_html_end - index_html_start);

    return httpd_resp_send(req, (const char *)index_html_start, index_html_size);
}

static esp_err_t stop_webserver() {
    ESP_LOGI(TAG, "stopping webserver");
    ESP_RETURN_ON_ERROR(httpd_stop(s_server), TAG, "httpd_stop failed");
    s_server = NULL;
    return ESP_OK;
}

static esp_err_t start_webserver() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // config.server_port = 8080;

    config.lru_purge_enable = true;
    config.max_open_sockets = 4;

    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    config.keep_alive_enable = false;

    config.stack_size = 4096;
    config.max_uri_handlers = 8;

    config.task_priority = tskIDLE_PRIORITY + 2;

    ESP_LOGI(TAG, "starting server on port: '%d'", config.server_port);
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start failed");
    DEFER(stop_webserver);

    static const httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &root_uri), TAG, "httpd_register_uri_handler failed");

    static const httpd_uri_t index_html_uri = {
        .uri = "/index.html", .method = HTTP_GET, .handler = index_html_get_handler};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &index_html_uri), TAG,
                        "httpd_register_uri_handler failed");

    return ESP_OK;
}

static esp_err_t app_logic() {
    ESP_RETURN_ON_ERROR(make_etag(s_etag, sizeof(s_etag)), TAG, "make_etag failed");
    ESP_LOGI(TAG, "ETag: %s", s_etag);

    ESP_RETURN_ON_ERROR(gpio_init(), TAG, "GPIO init failed");
    ESP_RETURN_ON_ERROR(nvs_init(), TAG, "NVS init failed");
    ESP_RETURN_ON_ERROR(wifi_init(), TAG, "WiFi init failed");
    ESP_RETURN_ON_ERROR(wifi_connect(), TAG, "WiFi connect failed");
    ESP_RETURN_ON_ERROR(start_webserver(), TAG, "start webserver failed");

    return blink_task(); // TODO: need graceful shutdown
}

void app_main(void) {
    ESP_ERROR_CHECK(closer_create(&s_closer));

    app_logic();

    closer_close(s_closer);
    closer_destroy(s_closer);
}
