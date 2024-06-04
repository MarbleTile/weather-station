
#include "driver/i2c_master.h"
#include "driver/i2c_types.h"
#include "esp_bit_defs.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_event_base.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_wifi_types_generic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "hal/i2c_types.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "portmacro.h"
#include "soc/gpio_num.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define INCLUDE_vTaskDelay 1
#define DBG 0
#define DBG_HTTP_EV 0

static const char* TAG = "weather";

#define ERR_CHECK(err, name) \
    if (err != ESP_OK) { \
        ESP_LOGE(TAG, "%s: %s", name, esp_err_to_name(err)); \
    } else if (DBG) { \
        ESP_LOGW(TAG, "%s: success!", name); \
    }

#define CTRL_PORT       I2C_NUM_0
#define CLK_TYPE        I2C_CLK_SRC_DEFAULT
#define GLITCH_TYPICAL  7
#define INTR_AUTO       0
#define SCL_GPIO        GPIO_NUM_8
#define SDA_GPIO        GPIO_NUM_10
#define SHTC3_ADDR      0x70
#define SHTC3_LEN       I2C_ADDR_BIT_LEN_7
#define SHTC3_CLK_HZ    400000
#define SHTC3_WAK       0x3517
#define SHTC3_SLP       0xb098
#define SHTC3_GET       0x7866
#define I2C_NOTIMEOUT   -1
#define CRC_POLY        0x31
#define CRC_INIT        0xff
#define SHTC3_CONV      65536.0
#define TEMP_CALIB      6.35

#define WIFI_SSID       "i kill children"
#define WIFI_PASS       "sodomy9000!"
#define WIFI_RETRY      5

#define WEATHER_IP      "192.168.1.80"

typedef struct i2c_params {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
} i2c_params_t;

void shtc3_probe(const i2c_params_t p) {
    ERR_CHECK(i2c_master_probe(p.bus, SHTC3_ADDR, I2C_NOTIMEOUT), \
            "shtc3_probe");
}

void shtc3_init(i2c_master_bus_handle_t* bus, i2c_master_dev_handle_t* dev) {
    i2c_master_bus_config_t bus_cfg = {0};
    bus_cfg = (i2c_master_bus_config_t) {
        .i2c_port           = CTRL_PORT,
        .sda_io_num         = SDA_GPIO,
        .scl_io_num         = SCL_GPIO,
        .clk_source         = CLK_TYPE,
        .glitch_ignore_cnt  = GLITCH_TYPICAL,
        .intr_priority      = INTR_AUTO,
        .flags              = {
            .enable_internal_pullup = 1
        }
    };

    ERR_CHECK(i2c_new_master_bus(&bus_cfg, bus), "i2c_new_master_bus");

    i2c_device_config_t dev_cfg = {0};
    dev_cfg = (i2c_device_config_t) {
        .dev_addr_length    = SHTC3_LEN,
        .device_address     = SHTC3_ADDR,
        .scl_speed_hz       = SHTC3_CLK_HZ
    };

    ERR_CHECK(i2c_master_bus_add_device(*bus, &dev_cfg, dev), \
            "i2c_master_bus_add_device");

    shtc3_probe((i2c_params_t){*bus, *dev});
}

void buf_mk(uint8_t* buf, const size_t sz, const uint32_t cmd) {
    for (size_t i = 0; i < sz; i++) {
        buf[i] = (cmd >> (8 * (sz - i - 1))) & 0xff;
    }
}

void buf_dbg(uint8_t*buf, const size_t sz, const char* name) {
    for (size_t i = 0; i < sz; i ++) {
        ESP_LOGW(TAG, "%s[%u] = 0x%x", name, i, buf[i]);
    }
}

void shtc3_cmd_wr(const i2c_params_t p, \
                  const uint32_t cmd, \
                  const char* name) {
    shtc3_probe(p);

    uint8_t buf[2];
    const size_t sz = sizeof (buf);
    buf_mk(buf, sz, cmd);
    if (DBG)
        buf_dbg(buf, sz, name);
    ERR_CHECK(i2c_master_transmit(p.dev, buf, sz, I2C_NOTIMEOUT), \
            "i2c_master_transmit");
}

void shtc3_cmd_rd(const i2c_params_t p, \
                  uint8_t* buf, \
                  const size_t buf_sz) {
    shtc3_probe(p);

    ERR_CHECK(i2c_master_receive(p.dev, buf, buf_sz, I2C_NOTIMEOUT), \
            "i2c_master_receive");
}

uint8_t crc_loop(uint8_t dat, uint8_t crc) {
    crc ^= dat;
    for (size_t i=0; i<8; i++) {
        crc = (crc & 0x80) == 0 ? crc<<1 : crc<<1 ^ CRC_POLY;
    }
    return crc;
}

bool crc_check(uint8_t top, uint8_t bot, uint8_t checksum) {
    uint8_t crc = CRC_INIT;
    return crc_loop(bot, crc_loop(top, crc)) == checksum;
}

void shtc3_measure(const i2c_params_t p, double* temp, double* humi) {
    uint8_t buf_rd[6] = {0};
    const size_t sz_rd = sizeof (buf_rd);

    shtc3_probe(p);

    shtc3_cmd_wr(p, SHTC3_GET, "measure");

    vTaskDelay(20/portTICK_PERIOD_MS); 

    shtc3_cmd_rd(p, buf_rd, sz_rd);

    if (DBG)
        buf_dbg(buf_rd, sz_rd, "dat");

    if (crc_check(buf_rd[0], buf_rd[1], buf_rd[2])) {
        *temp = (double) ((((buf_rd[0]<<8)+buf_rd[1])*175/ SHTC3_CONV - 45.0) - TEMP_CALIB);
    } else if (DBG) {
        ESP_LOGE(TAG, "temp crc fail!");
    }

    if (crc_check(buf_rd[3], buf_rd[4], buf_rd[5])) {
        *humi = (double) (((buf_rd[3]<<8)+buf_rd[4])*100/SHTC3_CONV);
    } else if (DBG) {
        ESP_LOGE(TAG, "humid crc fail!");
    }

}

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_ev_grp;
static int32_t s_retry_num = 0;

static void wifi_ev_handler(void* arg, esp_event_base_t event_base, \
        int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGW(TAG, "retry wifi connection");
        } else {
            xEventGroupSetBits(s_wifi_ev_grp, WIFI_FAIL_BIT);
        }
        ESP_LOGE(TAG, "wifi connection error");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* ev = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "ip:" IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_ev_grp, WIFI_CONNECTED_BIT);
    }
}

void wifi_init(void) {
    s_wifi_ev_grp = xEventGroupCreate();
    ERR_CHECK(esp_netif_init(), \
            "esp_netif_init");
    ERR_CHECK(esp_event_loop_create_default(), \
            "esp_event_loop_create_default");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ERR_CHECK(esp_wifi_init(&init_cfg), \
            "esp_wifi_init");

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ERR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, \
                ESP_EVENT_ANY_ID, &wifi_ev_handler, NULL, &instance_any_id), \
            "esp_event_handler_instance_register");
    ERR_CHECK(esp_event_handler_instance_register(IP_EVENT, \
                IP_EVENT_STA_GOT_IP, &wifi_ev_handler, NULL, &instance_got_ip), \
            "esp_event_handler_instance_register");

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_HASH_TO_ELEMENT
        }
    };
    ERR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA), \
            "esp_wifi_set_mode");
    ERR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), \
            "esp_wifi_set_config");
    ERR_CHECK(esp_wifi_start(), \
            "esp_wifi_start");

    if (DBG)
        ESP_LOGW(TAG, "wifi_init finished");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_ev_grp, \
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, \
            pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
        ESP_LOGI(TAG, "connected to %s", WIFI_SSID);
    else if (bits & WIFI_FAIL_BIT)
        ESP_LOGE(TAG, "failed to connect to %s", WIFI_SSID);
    else
        ESP_LOGE(TAG, "unexpected connection event");
}

#define HTTP_OUT_BUF_SZ 48

static char location[HTTP_OUT_BUF_SZ] = {0};

esp_err_t http_ev_handler(esp_http_client_event_t* ev) {
    switch (ev->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (DBG_HTTP_EV) {
                ESP_LOGW(TAG, "incoming response data:");
                int data_sz = ev->data_len;
                ESP_LOGW(TAG, "\tev->data_len: %d", data_sz);
                for (int i = 0; i < data_sz; i++) {
                    ESP_LOGW(TAG, "\tev->data[%d]: %"PRIx8"\t%c", \
                            i, ((char*)ev->data)[i], ((char*)ev->data)[i]);
                }
            }
            memset(ev->user_data, 0, HTTP_OUT_BUF_SZ);
            memcpy(ev->user_data, ev->data, ev->data_len);
            break;
        default:
            break;
    }
    return ESP_OK;
}

void http_init(void) {

    char loc[HTTP_OUT_BUF_SZ] = {0};
    esp_http_client_config_t loc_cfg = {0};
    loc_cfg = (esp_http_client_config_t) {
        .host = WEATHER_IP,
        .port = 1234,
        .path = "/location",
        .event_handler = http_ev_handler,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .method = HTTP_METHOD_GET,
        .disable_auto_redirect = true,
        .user_data = loc,
        .user_agent = "esp32c3",
    };
    esp_http_client_handle_t loc_client = esp_http_client_init(&loc_cfg);
    
    esp_http_client_set_header(loc_client, "Accept", "*/*");
    esp_http_client_set_header(loc_client, "Host", WEATHER_IP);

    esp_http_client_perform(loc_client);

    strcpy(location, loc);

    esp_http_client_cleanup(loc_client);
}

void http_get_weather(char* outtemp) {
    char wttr_data[HTTP_OUT_BUF_SZ] = {0};

    char get_path[HTTP_OUT_BUF_SZ*2];
    sprintf(get_path, "/%s?0Tm&format=%%t", location); 
    esp_http_client_config_t get_cfg = {0};
    get_cfg = (esp_http_client_config_t) {
        .host = "wttr.in",
        .port = 80,
        .path = get_path,
        .event_handler = http_ev_handler,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .method = HTTP_METHOD_GET,
        .disable_auto_redirect = false,
        .user_data = wttr_data,
        .user_agent = "curl/8.7.1",
    };
    esp_http_client_handle_t client = esp_http_client_init(&get_cfg);
    esp_http_client_set_header(client, "Accept", "text/html,text/plain,*/*");
    esp_http_client_set_header(client, "Accept-Language", "en-US,en");
    esp_http_client_set_header(client, "Connection", "keep-alive");
    esp_http_client_set_header(client, "Host", "wttr.in");

    esp_http_client_perform(client);

    if (esp_http_client_get_status_code(client) == HttpStatus_Ok) {
        if (DBG_HTTP_EV) {
            ESP_LOGW(TAG, "wttr status 200 OK");
            const size_t wttr_sz = strlen(wttr_data);
            ESP_LOGW(TAG, "\twttr_data length: %u", wttr_sz);
            for (size_t i = 0; i < wttr_sz; i++) {
                ESP_LOGW(TAG, "\twttr_data[%u] = %"PRIx8"\t%c", \
                        i, wttr_data[i], wttr_data[i]);
            }
        }
        strcpy(outtemp, wttr_data);
    } else if (DBG_HTTP_EV) {
        ESP_LOGW(TAG, "wttr status: %d", esp_http_client_get_status_code(client));
        strcpy(outtemp, "?");
    } else {
        strcpy(outtemp, "?");
    }

    esp_http_client_cleanup(client);
}

void http_post_weather(double temp, double humi) {
    char outtemp[HTTP_OUT_BUF_SZ];
    http_get_weather(outtemp);

    esp_http_client_config_t post_cfg = {0};
    post_cfg = (esp_http_client_config_t) {
        .host = WEATHER_IP,
        .port = 1234,
        .path = "/weather",
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .method = HTTP_METHOD_POST,
        .disable_auto_redirect = true,
        .user_agent = "esp32c3",
    };
    esp_http_client_handle_t client = esp_http_client_init(&post_cfg);
    esp_http_client_set_header(client, "Host", WEATHER_IP);

    char weather_data[HTTP_OUT_BUF_SZ*2];
    sprintf(weather_data, "localtemp=%4.2f&localhumi=%4.2f&outtemp=%s", \
            temp, humi, outtemp);
    if (DBG)
        ESP_LOGW(TAG, "post: %s", weather_data);
    esp_http_client_set_post_field(client, weather_data, strlen(weather_data));
    ERR_CHECK(esp_http_client_perform(client), \
            "esp_http_client_perform");

    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "location:\t%s\twttr.in temp:\t%s\tlocal temp:\t%4.2f°C\tlocal humidity:\t%4.2f%%", \
            location, outtemp, temp, humi);
}

void weather_task(void* pvParameters) {
    i2c_params_t p = *((i2c_params_t*) pvParameters);
    double temp = 0.0;
    double humi = 0.0;

    vTaskDelay(2000/portTICK_PERIOD_MS);
    for (;;) {
        shtc3_cmd_wr(p, SHTC3_WAK, "wakeup");
        shtc3_measure(p, &temp, &humi);
        shtc3_cmd_wr(p, SHTC3_SLP, "sleep");

        http_post_weather(temp, humi);

        vTaskDelay(2000/portTICK_PERIOD_MS);
    }
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ERR_CHECK(nvs_flash_erase(), "nvs_flash_erase");
        ERR_CHECK(nvs_flash_init(), "nvs_flash_init");
    }

    wifi_init();

    i2c_master_bus_handle_t bus = {0};
    i2c_master_dev_handle_t dev = {0};
    shtc3_init(&bus, &dev);
    i2c_params_t p = {
        .bus = bus,
        .dev = dev
    };

    http_init();

    xTaskCreate(weather_task, "weather_task", 8192, &p, 5, NULL);
}

