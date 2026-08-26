#include <sys/param.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_http_server.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/inet.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#include "iot_button.h"
#include "button_gpio.h"

#include <math.h>
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"

#include "dns_server.h"
#include "scan.h"
#include "ota.h"

#include "mpu6500.h"
#include "gsm4g.h"
#include "wifi_upload.h"

/* ============================ Pin map (per SCH-RD1.01) ============================ */
#define KEY1_GPIO         GPIO_NUM_46
#define KEY2_GPIO         GPIO_NUM_9

#define LED1_GPIO         GPIO_NUM_41
#define LED2_GPIO         GPIO_NUM_40
#define LED3_GPIO         GPIO_NUM_39
#define LED4_GPIO         GPIO_NUM_38

#define MPU6500_SDA_GPIO  GPIO_NUM_21
#define MPU6500_SCL_GPIO  GPIO_NUM_47

#define GSM4G_UART        UART_NUM_1
#define GSM4G_TX_GPIO     GPIO_NUM_1
#define GSM4G_RX_GPIO     GPIO_NUM_2
#define GSM4G_EN_GPIO     GPIO_NUM_45
#define GSM4G_RST_GPIO    GPIO_NUM_7

#define POWER_BTN_GPIO    GPIO_NUM_42    /* SW_PWR  */
#define POWER_HOLD_GPIO   GPIO_NUM_8     /* VCC_EN, drive HIGH to keep system on */

#define BATT_ADC_UNIT     ADC_UNIT_2     /* ADC2 */
#define BATT_ADC_CHANNEL  ADC_CHANNEL_6  /* GPIO17 = ADC2_CH6 (VCC_AD divider) */
#define BATT_DIVIDER_NUM  2              /* divider ratio numerator   */
#define BATT_DIVIDER_DEN  1              /* divider ratio denominator */

#define EXAMPLE_ESP_WIFI_SSID CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_MAX_STA_CONN  CONFIG_ESP_MAX_STA_CONN
#define EXAMPLE_ESP_MAXIMUM_RETRY 5
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER ""
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#define MAX_HTTP_OUTPUT_BUFFER 2048

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define POWER_LONG_PRESS_OFF_MS  3000

extern const char root_start[] asm("_binary_root_html_start");
extern const char root_end[]   asm("_binary_root_html_end");

extern char *get_wifi_list_json(void);

static const char *TAG = "FALLDETECT";
static uint8_t base_mac[6];

typedef enum {
    NET_NONE = 0,
    NET_WIFI = 1,
    NET_4G   = 2,
    NET_AP   = 3,
} net_mode_t;

typedef struct {
    char ssid[32];
    char password[64];
} wifi_credentials_t;

typedef struct {
    char url[128];
} url_credentials_t;

static wifi_credentials_t wifi_creds = { .ssid = "wifissid", .password = "wifipassword" };
static url_credentials_t url_creds   = { .url   = "192.168.1.31:8086" };

static EventGroupHandle_t s_wifi_event_group;
static volatile net_mode_t s_net_mode = NET_NONE;
static QueueHandle_t s_send_queue;

typedef enum {
    EVT_FALL = 1,
    EVT_KEY1 = 2,
    EVT_KEY2 = 3,
} send_event_t;

static httpd_handle_t start_webserver(void);
static void wifi_init_softap(void);
static void wifi_init_sta(void);

/* ============================ NVS persistence ============================ */
static void nvs_read(void)
{
    nvs_handle_t h;
    size_t len;
    if (nvs_open("wifi_config", NVS_READWRITE, &h) == ESP_OK) {
        len = sizeof(wifi_creds.ssid);
        nvs_get_str(h, "wifi_ssid", wifi_creds.ssid, &len);
        len = sizeof(wifi_creds.password);
        nvs_get_str(h, "wifi_password", wifi_creds.password, &len);
        nvs_close(h);
    }
    if (nvs_open("url_config", NVS_READWRITE, &h) == ESP_OK) {
        len = sizeof(url_creds.url);
        nvs_get_str(h, "url_config", url_creds.url, &len);
        nvs_close(h);
    }
}

static void nvs_write(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi_config", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "wifi_ssid", wifi_creds.ssid);
        nvs_set_str(h, "wifi_password", wifi_creds.password);
        nvs_commit(h);
        nvs_close(h);
    }
    if (nvs_open("url_config", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "url_config", url_creds.url);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ============================ Battery ADC ============================ */
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_adc_cali;
static bool                      s_adc_cali_ok;

static void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = BATT_ADC_UNIT };
    if (adc_oneshot_new_unit(&init_cfg, &s_adc) != ESP_OK) return;

    adc_oneshot_chan_cfg_t ch_cfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(s_adc, BATT_ADC_CHANNEL, &ch_cfg);

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BATT_ADC_UNIT,
        .chan     = BATT_ADC_CHANNEL,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_adc_cali_ok = (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_adc_cali) == ESP_OK);
#endif
}

static uint16_t battery_read_mv(void)
{
    if (!s_adc) return 0xFFFF;
    int raw = 0;
    if (adc_oneshot_read(s_adc, BATT_ADC_CHANNEL, &raw) != ESP_OK) return 0xFFFF;
    int mv = 0;
    if (s_adc_cali_ok && adc_cali_raw_to_voltage(s_adc_cali, raw, &mv) == ESP_OK) {
        return (uint16_t)(mv * BATT_DIVIDER_NUM / BATT_DIVIDER_DEN);
    }
    /* Fallback: 12-bit, 3.3V full scale at 12dB atten ~= 3100mV */
    mv = (raw * 3100) / 4095;
    return (uint16_t)(mv * BATT_DIVIDER_NUM / BATT_DIVIDER_DEN);
}

/* ============================ JSON build & POST ============================ */
static mpu6500_history_t s_pending_fall_hist;

static void build_payload(uint8_t btn, uint8_t state,
                          const mpu6500_history_t *hist,
                          char *out, size_t out_size)
{
    char roll_s[160], pitch_s[160], yaw_s[160];
    mpu6500_format_history(hist, roll_s, sizeof(roll_s),
                           pitch_s, sizeof(pitch_s), yaw_s, sizeof(yaw_s));

    gsm4g_gps_t gps = {0};
    gsm4g_get_gps(&gps);

    uint16_t bv = battery_read_mv();
    uint8_t cmd = (s_net_mode == NET_WIFI) ? 0x01 : 0x02;

    snprintf(out, out_size,
             "{\"CMD\":%u,\"ID\":\"%02x%02x%02x%02x%02x%02x\",\"BTN\":%u,"
             "\"LAT\":%.6f,\"LON\":%.6f,"
             "\"Roll\":\"%s\",\"Yaw\":\"%s\",\"Pitch\":\"%s\","
             "\"STATE\":%u,\"BV\":%u}",
             cmd,
             base_mac[0], base_mac[1], base_mac[2],
             base_mac[3], base_mac[4], base_mac[5],
             btn,
             gps.lat, gps.lon,
             roll_s, yaw_s, pitch_s,
             state, bv);
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        ESP_LOGD(TAG, "HTTP DATA len=%d", evt->data_len);
    }
    return ESP_OK;
}

static esp_err_t post_via_wifi(const char *json)
{
    char szUrl[200];
    snprintf(szUrl, sizeof(szUrl), "http://%s/rec/info", url_creds.url);

    esp_http_client_config_t cfg = {
        .url           = szUrl,
        .event_handler = http_event_cb,
        .timeout_ms    = 8000,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_FAIL;

    esp_http_client_set_method(cli, HTTP_METHOD_POST);
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, json, strlen(json));

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    ESP_LOGI(TAG, "WiFi POST -> status=%d err=%s", status, esp_err_to_name(err));
    esp_http_client_cleanup(cli);
    return (err == ESP_OK && status >= 200 && status < 300) ? ESP_OK : ESP_FAIL;
}

static esp_err_t post_via_4g(const char *json)
{
    char url[200];
    snprintf(url, sizeof(url), "http://%s/rec/info", url_creds.url);
    return gsm4g_http_post_json(url, json);
}

static void send_payload(uint8_t btn, uint8_t state, const mpu6500_history_t *hist)
{
    char body[1024];
    build_payload(btn, state, hist, body, sizeof(body));
    ESP_LOGI(TAG, "PAYLOAD %s", body);

    /* === 原有上传逻辑（兼容现有服务器） === */
    esp_err_t err = ESP_FAIL;
    if (s_net_mode == NET_WIFI) {
        err = post_via_wifi(body);
    } else if (gsm4g_is_ready()) {
        err = post_via_4g(body);
    }
    ESP_LOGI(TAG, "send result: %s", err == ESP_OK ? "OK" : "FAIL");

    /* === 新增：上传跌倒数据至 lele1129.top === */
    /* 构建标准跌倒数据格式 */
    fall_event_t fall_evt;
    fall_event_init(&fall_evt, "fall_detect_dev");

    /* 使用MAC地址作为后缀构成唯一device_id */
    snprintf(fall_evt.device_id, sizeof(fall_evt.device_id),
             "fall_%02x%02x%02x%02x%02x%02x",
             base_mac[0], base_mac[1], base_mac[2],
             base_mac[3], base_mac[4], base_mac[5]);

    /* 从历史数据计算倾斜角 */
    if (hist && hist->full) {
        /* 使用最新的角度数据计算倾斜角 */
        int idx = (hist->head == 0) ? (MPU6500_SAMPLE_HISTORY - 1) : (hist->head - 1);
        float roll  = hist->history[idx].roll;
        float pitch = hist->history[idx].pitch;
        /* 防御：传感器瞬间异常可能导致非有限值，回退为0避免NaN传播 */
        if (!isfinite(roll))  roll  = 0.0f;
        if (!isfinite(pitch)) pitch = 0.0f;
        /* 倾斜角 = arccos(cos(roll)*cos(pitch))，即设备相对竖直方向的倾角。
         * 使用 acos 公式而非 asin(sqrt(sin²r+sin²p))：后者的被开方数可达2，
         * sqrt≈1.414 超出 asinf 定义域 [-1,1]，躺平姿态(roll/pitch≈±90°)时会产生 NaN。 */
        float cr = cosf(roll * (float)M_PI / 180.0f);
        float cp = cosf(pitch * (float)M_PI / 180.0f);
        float dot = cr * cp;
        /* clamp 到 [-1,1]，防止浮点舍入误差导致 acosf 定义域越界 */
        dot = fminf(1.0f, fmaxf(-1.0f, dot));
        float total_angle = acosf(dot) * 180.0f / (float)M_PI;
        fall_evt.angle = total_angle;
        /* 近似加速度(用角度变化率估算) */
        fall_evt.acceleration = total_angle / 30.0f; /* 粗略换算 */
        if (fall_evt.acceleration > 20.0f) fall_evt.acceleration = 20.0f;
    }

    /* 从4G模块读取GPS坐标（如果可用） */
    gsm4g_gps_t gps = {0};
    if (gsm4g_get_gps(&gps)) {
        fall_evt.latitude  = gps.lat;
        fall_evt.longitude = gps.lon;
    } else {
        /* GPS不可用时使用默认坐标（后续可配置） */
        fall_evt.latitude  = 0.0;
        fall_evt.longitude = 0.0;
    }

    fall_evt.btn       = btn;
    fall_evt.state     = state;
    fall_evt.battery_mv = battery_read_mv();

    /* 根据网络模式选择上传路径 */
    if (s_net_mode == NET_WIFI) {
        /* WiFi模式：通过WiFi HTTPS异步上传到 lele1129.top */
        wifi_upload_event_async(&fall_evt, NULL, NULL);
    } else if (gsm4g_is_ready()) {
        /* 4G模式：4G模块就绪时通过其 HTTP POST 到 lele1129.top */
        /* 注意：不检查 s_net_mode == NET_4G，因为4G可能后期才就绪 */
        char json_buf[JSON_BUF_SIZE];
        int len = fall_event_to_json(&fall_evt, json_buf, sizeof(json_buf));
        if (len > 0) {
            /* 4G模块AT指令方式使用HTTP而非HTTPS */
            esp_err_t gsm_err = gsm4g_http_post_json(LELE1129_API_URL_HTTP, json_buf);
            if (gsm_err == ESP_OK) {
                ESP_LOGI(TAG, "4G upload to lele1129.top OK");
            } else {
                ESP_LOGE(TAG, "4G upload to lele1129.top FAIL: %s", esp_err_to_name(gsm_err));
            }
        }
    }
}

/* ============================ Send dispatch task ============================ */
static void send_task(void *arg)
{
    send_event_t evt;
    mpu6500_history_t hist;
    while (true) {
        if (xQueueReceive(s_send_queue, &evt, portMAX_DELAY) == pdTRUE) {
            switch (evt) {
            case EVT_FALL:
                send_payload(1, 1, &s_pending_fall_hist);
                break;
            case EVT_KEY1:
                mpu6500_get_history(&hist);
                send_payload(1, 0, &hist);
                break;
            case EVT_KEY2:
                mpu6500_get_history(&hist);
                send_payload(2, 0, &hist);
                break;
            }
        }
    }
}

/* ============================ Fall watcher ============================ */
static void fall_watcher_task(void *arg)
{
    while (true) {
        if (mpu6500_take_fall_event(&s_pending_fall_hist)) {
            send_event_t e = EVT_FALL;
            xQueueSend(s_send_queue, &e, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ============================ LEDs ============================ */
static void led_init(void)
{
    const gpio_num_t leds[] = { LED1_GPIO, LED2_GPIO, LED3_GPIO, LED4_GPIO };
    for (size_t i = 0; i < sizeof(leds) / sizeof(leds[0]); i++) {
        gpio_reset_pin(leds[i]);
        gpio_set_direction(leds[i], GPIO_MODE_OUTPUT);
        gpio_set_level(leds[i], 0);
    }
    
    gpio_set_level(LED4_GPIO, 1);
}

static void led_status_task(void *arg)
{
    while (true) {
        switch (s_net_mode) {
        case NET_WIFI:
            gpio_set_level(LED1_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
            gpio_set_level(LED1_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case NET_4G:
            gpio_set_level(LED2_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
            gpio_set_level(LED2_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(1000));
            break;
        case NET_AP:
            gpio_set_level(LED3_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(150));
            gpio_set_level(LED3_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(150));
            break;
        default:
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        }
    }
}

/* ============================ Power button (one-key power) ============================ */
static void power_hold_assert(void)
{
    gpio_reset_pin(POWER_HOLD_GPIO);
    gpio_set_direction(POWER_HOLD_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(POWER_HOLD_GPIO, 1);
}

static void power_off(void)
{
    ESP_LOGW(TAG, "Power off requested");
    for (int i = 0; i < 4; i++) {
        gpio_set_level(LED1_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(LED1_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    gpio_set_level(POWER_HOLD_GPIO, 0); /* drop VCC_EN -> system off */
}

static void power_btn_cb(void *arg, void *data)
{
    button_event_t ev = iot_button_get_event((button_handle_t)arg);
    if (ev == BUTTON_LONG_PRESS_START) {
        power_off();
    }
}

static void power_button_init(void)
{
    button_config_t btn_cfg = {
        .long_press_time = POWER_LONG_PRESS_OFF_MS,
    };
    button_gpio_config_t gpio_cfg = {
        .gpio_num     = POWER_BTN_GPIO,
        .active_level = 0,
    };
    button_handle_t btn;
    if (iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn) == ESP_OK) {
        iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, power_btn_cb, NULL);
    }
}

/* ============================ Trigger (KEY1/KEY2) buttons ============================ */

// ---- 以下代码来源于 main/main.c:406-414 ----

// 按键回调函数：判断按下了哪个按键，然后向发送队列投递对应的事件
static void key_btn_cb(void* arg, void* data)
{
    // 第1步：获取按键事件类型（按下、抬起、长按等）
    button_event_t ev;
    ev = iot_button_get_event((button_handle_t)arg);

    // 第2步：如果不是"按下"事件，就忽略，直接返回
    if (ev != BUTTON_PRESS_DOWN) {
        return;
    }

    // 第3步：从参数 data 中取出 GPIO 编号，判断是 KEY1 还是 KEY2
    uint32_t gpio_num;
    gpio_num = (uint32_t)data;

    // 第4步：根据 GPIO 编号选择对应的事件类型
    send_event_t e;
    if (gpio_num == KEY1_GPIO) {
        // 如果按下的是 KEY1，就投递 EVT_KEY1 事件
        e = EVT_KEY1;
    } else {
        // 否则就是 KEY2，投递 EVT_KEY2 事件
        e = EVT_KEY2;
    }

    // 第5步：把事件发送到消息队列（第3个参数 0 表示不等待，队列满就直接丢弃）
    xQueueSend(s_send_queue, &e, 0);
}
static void key_buttons_init(void)
{
    const uint32_t pins[] = { KEY1_GPIO, KEY2_GPIO };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        button_config_t btn_cfg = {0};
        button_gpio_config_t gpio_cfg = {
            .gpio_num         = pins[i],
            .active_level     = 0,
            .enable_power_save = false,
        };
        button_handle_t btn;
        if (iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn) == ESP_OK) {
            iot_button_register_cb(btn, BUTTON_PRESS_DOWN, NULL, key_btn_cb, (void *)pins[i]);
        }
    }
}

/* ============================ WiFi STA + AP fallback ============================ */
static int s_retry_num = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void ap_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)data;
        ESP_LOGI(TAG, "AP join " MACSTR " AID=%d", MAC2STR(ev->mac), ev->aid);
    }
}

static void wifi_init_common(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();

    esp_event_handler_instance_t any_id, got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,    &wifi_event_handler, NULL, &any_id);
    esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e        = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
        },
    };
    strncpy((char *)wifi_config.sta.ssid,     wifi_creds.ssid,     sizeof(wifi_config.sta.ssid)     - 1);
    strncpy((char *)wifi_config.sta.password, wifi_creds.password, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta started, waiting up to 15s...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected: %s", wifi_creds.ssid);
        s_net_mode = NET_WIFI;
    } else {
        ESP_LOGW(TAG, "WiFi failed/timeout");
        /* Caller will fall through to 4G mode. */
    }
}

static void wifi_init_softap(void)
{
    s_net_mode = NET_AP;

    esp_netif_create_default_wifi_ap();
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &ap_event_handler, NULL, NULL);

    wifi_config_t cfg = {
        .ap = {
            .ssid           = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len       = strlen(EXAMPLE_ESP_WIFI_SSID),
            .password       = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
            .authmode       = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_webserver();
}

/* ============================ Captive portal HTTP ============================ */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, root_start, root_end - root_start);
    return ESP_OK;
}

static esp_err_t wifi_list_get_handler(httpd_req_t *req)
{
    char *json = get_wifi_list_json();
    if (!json) { httpd_resp_send_500(req); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);
    return ESP_OK;
}

static esp_err_t creds_set_handler(httpd_req_t *req)
{
    char content[1024];
    int recv = httpd_req_recv(req, content, MIN(req->content_len, sizeof(content) - 1));
    if (recv <= 0) { httpd_resp_send_500(req); return ESP_FAIL; }
    content[recv] = '\0';

    cJSON *root = cJSON_Parse(content);
    if (!root) { httpd_resp_send_500(req); return ESP_FAIL; }

    cJSON *ssid_i = cJSON_GetObjectItem(root, "ssid");
    cJSON *pwd_i  = cJSON_GetObjectItem(root, "password");
    cJSON *url_i  = cJSON_GetObjectItem(root, "serviceurl");
    if (ssid_i && pwd_i && url_i) {
        strncpy(wifi_creds.ssid,     ssid_i->valuestring, sizeof(wifi_creds.ssid)     - 1);
        strncpy(wifi_creds.password, pwd_i->valuestring,  sizeof(wifi_creds.password) - 1);
        strncpy(url_creds.url,       url_i->valuestring,  sizeof(url_creds.url)       - 1);
        nvs_write();
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":true}");
    } else {
        httpd_resp_send_err(req, 400, "Missing fields");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t creds_get_handler(httpd_req_t *req)
{
    char buf[384];
    snprintf(buf, sizeof(buf),
             "{\"creds_config\":[{\"ssid\":\"%s\",\"password\":\"%s\",\"url\":\"%s\"}]}",
             wifi_creds.ssid, wifi_creds.password, url_creds.url);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t gps_get_handler(httpd_req_t *req)
{
    gsm4g_gps_t gps = {0};
    bool got = gsm4g_get_gps(&gps);
    char buf[256];
    if (got) {
        snprintf(buf, sizeof(buf),
                 "{\"valid\":true,\"lat\":%.6f,\"lon\":%.6f,\"msg\":\"GPS fixed\"}",
                 gps.lat, gps.lon);
    } else {
        snprintf(buf, sizeof(buf),
                 "{\"valid\":false,\"lat\":0,\"lon\":0,\"msg\":\"GPS not fixed yet - check antenna or wait for satellite lock\"}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t http_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "redirect", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 13;
    cfg.lru_purge_enable = true;
    if (httpd_start(&server, &cfg) != ESP_OK) return NULL;

    httpd_uri_t r = { .uri = "/",                       .method = HTTP_GET,  .handler = root_get_handler,      .user_ctx = NULL };
    httpd_uri_t l = { .uri = "/api/wifi_list",          .method = HTTP_GET,  .handler = wifi_list_get_handler, .user_ctx = NULL };
    httpd_uri_t s = { .uri = "/api/creds_set_config",   .method = HTTP_POST, .handler = creds_set_handler,     .user_ctx = NULL };
    httpd_uri_t g = { .uri = "/api/creds_get_config",   .method = HTTP_GET,  .handler = creds_get_handler,     .user_ctx = NULL };
    httpd_uri_t gps = { .uri = "/api/gps",               .method = HTTP_GET,  .handler = gps_get_handler,      .user_ctx = NULL };
    httpd_uri_t b   = { .uri = "/api/reboot_post_config", .method = HTTP_POST, .handler = reboot_handler,        .user_ctx = NULL };
    httpd_register_uri_handler(server, &r);
    httpd_register_uri_handler(server, &l);
    httpd_register_uri_handler(server, &s);
    httpd_register_uri_handler(server, &g);
    httpd_register_uri_handler(server, &gps);
    httpd_register_uri_handler(server, &b);
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_handler);
    return server;
}

/* ============================ SNTP时间同步 ============================ */
static void initialize_sntp(void)
{
    esp_sntp_stop();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_init();

    ESP_LOGI(TAG, "SNTP: waiting for time sync...");
    time_t now = 0;
    struct tm tm = {0};
    int retry = 0;
    while (tm.tm_year < (2025 - 1900) && retry < 30) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        time(&now);
        localtime_r(&now, &tm);
        retry++;
    }
    if (retry < 30) {
        ESP_LOGI(TAG, "SNTP synced: %s", asctime(&tm));
    } else {
        ESP_LOGW(TAG, "SNTP timeout, using local RTC");
    }
}

/* ============================ app_main ============================ */
void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    power_hold_assert();   /* keep VCC_EN high so the system stays on after release */
    led_init();
    battery_init();

    nvs_read();
    esp_base_mac_addr_get(base_mac);
    ESP_LOGI(TAG, "MAC %02x:%02x:%02x:%02x:%02x:%02x",
             base_mac[0], base_mac[1], base_mac[2],
             base_mac[3], base_mac[4], base_mac[5]);

    s_send_queue = xQueueCreate(8, sizeof(send_event_t));

    /* Sensors */
    if (mpu6500_init(MPU6500_SDA_GPIO, MPU6500_SCL_GPIO) == ESP_OK) {
        mpu6500_start_task();
    } else {
        ESP_LOGE(TAG, "MPU-6500 init failed");
    }

    /* Buttons */
    key_buttons_init();
    power_button_init();

    /* Networking: try WiFi first; only start 4G if WiFi fails */
    wifi_init_common();
    wifi_init_sta();

    /* SNTP时间同步（需要WiFi连接）*/
    if (s_net_mode == NET_WIFI) {
        initialize_sntp();
    }

    /* 初始化上传模块（WiFi可用时复用；断线时自动重连）*/
    wifi_upload_init(NULL, NULL);

    if (s_net_mode != NET_WIFI) {
        ESP_LOGW(TAG, "WiFi unavailable, switching to 4G mode");
        s_net_mode = NET_4G;
        gsm4g_init(GSM4G_UART, GSM4G_TX_GPIO, GSM4G_RX_GPIO,
                   GSM4G_EN_GPIO, GSM4G_RST_GPIO);
        for (int i = 0; i < 60 && !gsm4g_is_ready(); i++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (!gsm4g_is_ready()) {
            ESP_LOGE(TAG, "4G failed to come up; entering AP/captive-portal mode");
            esp_wifi_stop();
            wifi_init_softap();
            start_dns_server();
        }
    } else {
        ESP_LOGI(TAG, "WiFi up, starting GPS-only mode for 4G module");
        gsm4g_gps_only_init(GSM4G_UART, GSM4G_TX_GPIO, GSM4G_RX_GPIO,
                            GSM4G_EN_GPIO, GSM4G_RST_GPIO);
    }

    xTaskCreate(send_task,        "send",        6144, NULL, 5, NULL);
    xTaskCreate(fall_watcher_task,"fall_watch",  3072, NULL, 6, NULL);
    xTaskCreate(led_status_task,  "led_status",  2048, NULL, 3, NULL);
}
