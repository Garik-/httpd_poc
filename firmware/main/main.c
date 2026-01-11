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
#include "mdns.h"
#include "nvs_flash.h"

#define LED_PIN GPIO_NUM_8
#define LED_BLINK_INTERVAL pdMS_TO_TICKS(512)

#define WAIT_STA_GOT_IP_MAX pdMS_TO_TICKS(10000) // TODO: make configurable

#define GPIO_OUTPUT_PIN_SEL ((1ULL << LED_PIN))

static const char *TAG = "httpd_poc";

#define CLOSER_IMPLEMENTATION
#include "closer.h"

static closer_handle_t s_closer = NULL;
#define DEFER(fn) CLOSER_DEFER(s_closer, (void *)fn)

static esp_netif_t *s_sta_netif = NULL;
static httpd_handle_t s_server = NULL;

static TaskHandle_t xTaskToNotify = NULL;

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

static esp_err_t api_led_set_level(httpd_req_t *req, uint32_t level) {
    esp_err_t err = gpio_set_level(LED_PIN, level);

    if (unlikely(err != ESP_OK)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }

    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t api_led_post_on_handler(httpd_req_t *req) {
    return api_led_set_level(req, 0);
}

static esp_err_t api_led_post_off_handler(httpd_req_t *req) {
    return api_led_set_level(req, 1);
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

static void handler_on_sta_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    if (event->esp_netif != s_sta_netif) {
        ESP_LOGW(TAG, "Got IP event for unknown netif");
        return;
    }

    ESP_LOGI(TAG, "Got IPv4 event, address: " IPSTR, IP2STR(&event->ip_info.ip));

    TaskHandle_t to_notify = __atomic_load_n(&xTaskToNotify, __ATOMIC_SEQ_CST);
    if (to_notify) {
        if (xPortInIsrContext()) {
            BaseType_t xHigherPriorityTaskWoken = pdFALSE;
            xTaskNotifyFromISR(to_notify, 0, eSetValueWithOverwrite, &xHigherPriorityTaskWoken);
            if (xHigherPriorityTaskWoken) {
                portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
            }
        } else {
            xTaskNotify(to_notify, 0, eSetValueWithOverwrite);
        }
    }
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

    __atomic_store_n(&xTaskToNotify, xTaskGetCurrentTaskHandle(), __ATOMIC_SEQ_CST);

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &handler_on_sta_got_ip, NULL), TAG,
                        "esp_event_handler_register failed");

    esp_err_t err = esp_wifi_connect();

    if (err != ESP_OK) {
        goto cleanup;
    }

    ESP_LOGI(TAG, "Waiting for IP address...");

    if (xTaskNotifyWait(pdFALSE, ULONG_MAX, NULL, WAIT_STA_GOT_IP_MAX) != pdPASS) {
        err = ESP_ERR_TIMEOUT;
        ESP_LOGW(TAG, "No ip received within the timeout period");

        goto cleanup;
    }

    err = ESP_OK;

cleanup:

    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &handler_on_sta_got_ip); // TODO: спорно
    __atomic_store_n(&xTaskToNotify, NULL, __ATOMIC_SEQ_CST);

    return err;
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
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, must-revalidate");

    // "no-cache, must-revalidate" - for dynamic content
    // "public, max-age=300, s-maxage=86400, stale-while-revalidate=300, stale-if-error=3600" - for static files
    // behind "public, max-age=31536000, immutable" - for versioned static files

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

    config.server_port = CONFIG_HTTPD_HTTP_PORT;

    config.lru_purge_enable = true;
    config.max_open_sockets = 4;

    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    config.keep_alive_enable = true;

    config.stack_size = 6144;
    config.max_uri_handlers = 8;

    config.task_priority = tskIDLE_PRIORITY + 3;

    ESP_LOGI(TAG, "starting server on port: '%d'", config.server_port);
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "httpd_start failed");
    DEFER(stop_webserver);

    static const httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = root_get_handler};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &root_uri), TAG, "httpd_register_uri_handler failed");

    static const httpd_uri_t index_html_uri = {
        .uri = "/index.html", .method = HTTP_GET, .handler = index_html_get_handler};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &index_html_uri), TAG,
                        "httpd_register_uri_handler failed");

    static const httpd_uri_t api_led_post_on = {
        .uri = "/api/led/on", .method = HTTP_POST, .handler = api_led_post_on_handler};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &api_led_post_on), TAG,
                        "httpd_register_uri_handler failed");

    static const httpd_uri_t api_led_post_off = {
        .uri = "/api/led/off", .method = HTTP_POST, .handler = api_led_post_off_handler};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &api_led_post_off), TAG,
                        "httpd_register_uri_handler failed");

    return ESP_OK;
}

static esp_err_t mdns_start() {
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns_init failed");
    DEFER(mdns_free);

    ESP_RETURN_ON_ERROR(mdns_hostname_set(CONFIG_HTTPD_MDNS_NAME), TAG, "mdns_hostname_set failed");
    ESP_LOGI(TAG, "mdns hostname set to: [%s]", CONFIG_HTTPD_MDNS_NAME);

    ESP_RETURN_ON_ERROR(mdns_instance_name_set("ESP32 with mDNS"), TAG,
                        "mdns_instance_name_set failed"); // TODO: make configurable
    ESP_RETURN_ON_ERROR(mdns_service_add(NULL, "_http", "_tcp", CONFIG_HTTPD_HTTP_PORT, NULL, 0), TAG,
                        "mdns_service_add failed");

    return ESP_OK;
}

static esp_err_t app_logic() {
    ESP_RETURN_ON_ERROR(make_etag(s_etag, sizeof(s_etag)), TAG, "make_etag failed");
    ESP_LOGI(TAG, "ETag: %s", s_etag);

    ESP_RETURN_ON_ERROR(gpio_init(), TAG, "GPIO init failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(LED_PIN, 1), TAG, "gpio_set_level failed"); // LED off

    ESP_RETURN_ON_ERROR(nvs_init(), TAG, "NVS init failed");
    ESP_RETURN_ON_ERROR(wifi_init(), TAG, "WiFi init failed");
    ESP_RETURN_ON_ERROR(wifi_connect(), TAG, "WiFi connect failed");
    ESP_RETURN_ON_ERROR(mdns_start(), TAG, "mDNS init failed");
    ESP_RETURN_ON_ERROR(start_webserver(), TAG, "start webserver failed");

    return ESP_OK;
}

void app_main(void) {
    ESP_ERROR_CHECK(closer_create(&s_closer));

    esp_err_t err = app_logic();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "application error: %s", esp_err_to_name(err));
        closer_close(s_closer);
        closer_destroy(s_closer);
    }
}
