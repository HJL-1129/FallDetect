/**
 * @file wifi_upload.c
 * @brief WiFi HTTP上传模块实现
 * 
 * 使用 ESP-IDF esp_http_client 通过HTTPS POST上传跌倒事件数据。
 * 
 * 设计原则:
 * - 利用现有WiFi连接（由 main.c 的 wifi_init_sta 建立）
 * - 监控WiFi状态，断线时自动重连（无限重试，间隔3秒）
 * - 上传失败自动重试3次
 * - 异步上传，不阻塞主循环
 * - 复用FreeRTOS队列机制，与现有 send_task 兼容
 */

#include "wifi_upload.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#define TAG "WIFI_UPLOAD"

/* 当HTTPS失败时是否允许自动回退到HTTP */
#define UPLOAD_HTTP_FALLBACK_ENABLED  true

/* 上传使用的URL（非HTTPS，用于回退） */
#define UPLOAD_SERVER_URL_HTTP      "http://lele1129.top"
#define UPLOAD_SERVER_URL_HTTPS     "https://lele1129.top"

/* WiFi事件位 */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_DISCONNECT_BIT BIT1

/* 上传队列项 */
typedef struct {
    fall_event_t       event;      /* 拷贝的跌倒事件数据 */
    upload_callback_t  callback;   /* 上传完成回调 */
    void              *user_data;  /* 回调用户数据 */
} upload_queue_item_t;

/* 模块内部状态 */
static struct {
    bool               initialized;           /* 是否已初始化 */
    EventGroupHandle_t wifi_event_group;       /* WiFi事件组 */
    wifi_state_t       state;                 /* 当前WiFi状态 */
    QueueHandle_t      upload_queue;           /* 上传队列 */
    TaskHandle_t       upload_task_handle;     /* 上传任务句柄 */
    uint32_t           reconnect_interval_ms;  /* 重连间隔(ms) */
    bool               keep_running;           /* 运行标志 */
} s_mod = {
    .initialized          = false,
    .wifi_event_group     = NULL,
    .state                = WIFI_STATE_DISCONNECTED,
    .upload_queue         = NULL,
    .upload_task_handle   = NULL,
    .reconnect_interval_ms = WIFI_RECONNECT_MS,
    .keep_running         = false,
};

/* ============================ WiFi事件处理 ============================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected");
        s_mod.state = WIFI_STATE_DISCONNECTED;
        if (s_mod.wifi_event_group) {
            xEventGroupSetBits(s_mod.wifi_event_group, WIFI_DISCONNECT_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_mod.state = WIFI_STATE_CONNECTED;
        if (s_mod.wifi_event_group) {
            xEventGroupSetBits(s_mod.wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

/* ============================ HTTP上传实现 ============================ */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        ESP_LOGD(TAG, "HTTP response: %.*s",
                 evt->data_len, (char *)evt->data);
    }
    return ESP_OK;
}

/**
 * @brief 执行一次HTTP POST上传
 * 
 * @param json_buf JSON数据字符串
 * @param status_code 输出HTTP状态码
 * @return true 上传成功, false 上传失败
 */
static bool http_post_json(const char *json_buf, int *status_code)
{
    if (!json_buf || !status_code) return false;

    /* 构建完整URL */
    char url[512];
    snprintf(url, sizeof(url), "%s%s", UPLOAD_SERVER_URL, UPLOAD_SERVER_PATH);

    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = http_event_handler,
        .timeout_ms    = 10000,                     /* 10秒超时 */
        .skip_cert_common_name_check = true,        /* 跳过证书CN检查提高兼容性 */
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        *status_code = 0;
        return false;
    }

    /* 设置POST方法和头 */
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_buf, strlen(json_buf));

    /* 执行请求 */
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    *status_code = status;

    bool success = (err == ESP_OK && status >= 200 && status < 300);

    if (success) {
        ESP_LOGI(TAG, "Upload OK, HTTP %d", status);
        esp_http_client_cleanup(client);
        return true;
    }

    /* HTTPS可能因证书问题失败，记录错误码供诊断 */
    ESP_LOGW(TAG, "Upload via %s FAIL: err=%s, HTTP %d", url,
             esp_err_to_name(err), status);
    esp_http_client_cleanup(client);
    return false;
}

/**
 * @brief 执行一次WiFi重连
 */
static bool do_wifi_reconnect(void)
{
    if (s_mod.state == WIFI_STATE_CONNECTING) {
        /* 已在连接中 */
        return false;
    }

    s_mod.state = WIFI_STATE_CONNECTING;

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        s_mod.state = WIFI_STATE_ERROR;
        return false;
    }

    /* 等待连接成功（最多10秒） */
    EventBits_t bits = xEventGroupWaitBits(
        s_mod.wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_DISCONNECT_BIT,
        pdTRUE,    /* 清除位 */
        pdFALSE,   /* 任意位满足即可 */
        pdMS_TO_TICKS(10000)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        s_mod.state = WIFI_STATE_CONNECTED;
        return true;
    }

    ESP_LOGW(TAG, "Reconnect timeout");
    s_mod.state = WIFI_STATE_DISCONNECTED;
    return false;
}

/* ============================ 上传任务 ============================ */

static void upload_task_func(void *arg)
{
    (void)arg;
    upload_queue_item_t item;

    ESP_LOGI(TAG, "Upload task started");

    while (s_mod.keep_running) {
        /* 从队列接收上传请求（超时1000ms以便检查keep_running）*/
        BaseType_t received = xQueueReceive(
            s_mod.upload_queue, &item, pdMS_TO_TICKS(1000));

        if (received != pdTRUE) {
            continue;
        }

        /* 检查WiFi状态，如果断线则等待重连 */
        while (s_mod.state != WIFI_STATE_CONNECTED && s_mod.keep_running) {
            ESP_LOGW(TAG, "WiFi not connected, reconnecting...");
            do_wifi_reconnect();

            if (s_mod.state != WIFI_STATE_CONNECTED) {
                vTaskDelay(pdMS_TO_TICKS(s_mod.reconnect_interval_ms));
            }
        }

        if (!s_mod.keep_running) {
            break;
        }

        /* 序列化为JSON */
        char json_buf[JSON_BUF_SIZE];
        int len = fall_event_to_json(&item.event, json_buf, sizeof(json_buf));
        if (len < 0) {
            ESP_LOGE(TAG, "JSON serialize failed");
            if (item.callback) {
                item.callback(false, 0, item.user_data);
            }
            continue;
        }

        ESP_LOGI(TAG, "Uploading: %s", json_buf);

        /* 重试上传 */
        bool success = false;
        int status_code = 0;

        /* 第一次尝试：HTTPS上传 */
        ESP_LOGI(TAG, "Attempting HTTPS upload...");
        for (int retry = 0; retry < UPLOAD_MAX_RETRIES; retry++) {
            if (retry > 0) {
                ESP_LOGW(TAG, "HTTPS retry %d/%d...", retry + 1, UPLOAD_MAX_RETRIES);
                vTaskDelay(pdMS_TO_TICKS(1000 * retry));
            }

            success = http_post_json(json_buf, &status_code);
            if (success) {
                break;
            }

            if (s_mod.state != WIFI_STATE_CONNECTED) {
                do_wifi_reconnect();
            }
        }

        /* HTTPS失败时，如果启用HTTP回退，尝试HTTP */
        if (!success && UPLOAD_HTTP_FALLBACK_ENABLED) {
            ESP_LOGW(TAG, "HTTPS failed, trying HTTP fallback...");
            /* 临时修改URL为HTTP并重试 */
            for (int retry = 0; retry < UPLOAD_MAX_RETRIES; retry++) {
                if (retry > 0) {
                    ESP_LOGW(TAG, "HTTP retry %d/%d...", retry + 1, UPLOAD_MAX_RETRIES);
                    vTaskDelay(pdMS_TO_TICKS(1000 * retry));
                }

                /* 用HTTP URL上传 */
                char http_url[512];
                snprintf(http_url, sizeof(http_url), "%s%s",
                         UPLOAD_SERVER_URL_HTTP, UPLOAD_SERVER_PATH);
                /* 复用http_post_json需要临时修改URL，直接构建HTTP请求 */
                esp_http_client_config_t cfg = {
                    .url           = http_url,
                    .event_handler = http_event_handler,
                    .timeout_ms    = 10000,
                };
                esp_http_client_handle_t client = esp_http_client_init(&cfg);
                if (client) {
                    esp_http_client_set_method(client, HTTP_METHOD_POST);
                    esp_http_client_set_header(client, "Content-Type", "application/json");
                    esp_http_client_set_post_field(client, json_buf, strlen(json_buf));
                    esp_err_t err = esp_http_client_perform(client);
                    status_code = esp_http_client_get_status_code(client);
                    success = (err == ESP_OK && status_code >= 200 && status_code < 300);
                    ESP_LOGI(TAG, "HTTP fallback result: err=%s status=%d success=%d",
                             esp_err_to_name(err), status_code, success);
                    esp_http_client_cleanup(client);
                    if (success) break;
                }
                if (s_mod.state != WIFI_STATE_CONNECTED) {
                    do_wifi_reconnect();
                }
            }
        }

        if (success) {
            ESP_LOGI(TAG, "Upload success");
        } else {
            ESP_LOGE(TAG, "Upload failed after %d retries", UPLOAD_MAX_RETRIES);
        }

        /* 回调通知 */
        if (item.callback) {
            item.callback(success, status_code, item.user_data);
        }
    }

    ESP_LOGI(TAG, "Upload task stopped");
    s_mod.upload_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ============================ 公共API ============================ */

bool wifi_upload_init(const char *ssid, const char *password)
{
    if (s_mod.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }

    (void)ssid;    /* WiFi由 main.c 的 wifi_init_sta 建立 */
    (void)password;

    /* 注册WiFi事件监听 */
    s_mod.wifi_event_group = xEventGroupCreate();
    if (!s_mod.wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return false;
    }

    /* 注册事件处理器 */
    esp_event_handler_instance_t any_id;
    esp_event_handler_instance_t got_ip;
    esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, &any_id);
    esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, &got_ip);

    /* 创建上传队列（最大8项） */
    s_mod.upload_queue = xQueueCreate(8, sizeof(upload_queue_item_t));
    if (!s_mod.upload_queue) {
        ESP_LOGE(TAG, "Failed to create upload queue");
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &any_id);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &got_ip);
        vEventGroupDelete(s_mod.wifi_event_group);
        s_mod.wifi_event_group = NULL;
        return false;
    }

    /* 检查当前WiFi状态 */
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        s_mod.state = WIFI_STATE_CONNECTED;
        ESP_LOGI(TAG, "WiFi already connected to %s", ap_info.ssid);
        xEventGroupSetBits(s_mod.wifi_event_group, WIFI_CONNECTED_BIT);
    } else {
        s_mod.state = WIFI_STATE_DISCONNECTED;
        ESP_LOGW(TAG, "WiFi not connected, will reconnect when needed");
    }

    /* 创建上传任务 */
    s_mod.keep_running = true;
    BaseType_t task_created = xTaskCreate(
        upload_task_func,
        "wifi_upload",
        4096,       /* 4KB栈 */
        NULL,
        4,          /* 优先级4 */
        &s_mod.upload_task_handle
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create upload task");
        vQueueDelete(s_mod.upload_queue);
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &any_id);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &got_ip);
        vEventGroupDelete(s_mod.wifi_event_group);
        s_mod.upload_queue = NULL;
        s_mod.wifi_event_group = NULL;
        return false;
    }

    s_mod.initialized = true;
    ESP_LOGI(TAG, "Initialized");
    return true;
}

wifi_state_t wifi_upload_get_state(void)
{
    return s_mod.state;
}

bool wifi_upload_event_async(const fall_event_t *event,
                              upload_callback_t callback,
                              void *user_data)
{
    if (!s_mod.initialized || !s_mod.upload_queue) {
        ESP_LOGE(TAG, "Not initialized");
        return false;
    }

    if (!event) {
        ESP_LOGE(TAG, "NULL event pointer");
        return false;
    }

    upload_queue_item_t item;
    memcpy(&item.event, event, sizeof(fall_event_t));
    item.callback   = callback;
    item.user_data  = user_data;

    BaseType_t sent = xQueueSend(s_mod.upload_queue, &item, 0);
    if (sent != pdTRUE) {
        ESP_LOGW(TAG, "Upload queue full, dropping event");
        return false;
    }

    ESP_LOGI(TAG, "Queued upload for %s", event->device_id);
    return true;
}

bool wifi_upload_event_sync(const fall_event_t *event)
{
    if (!event) return false;

    /* 序列化为JSON */
    char json_buf[JSON_BUF_SIZE];
    int len = fall_event_to_json(event, json_buf, sizeof(json_buf));
    if (len < 0) {
        ESP_LOGE(TAG, "JSON serialize failed");
        return false;
    }

    /* 确保WiFi已连接 */
    if (s_mod.state != WIFI_STATE_CONNECTED) {
        ESP_LOGW(TAG, "WiFi not connected, attempting reconnect...");
        if (!do_wifi_reconnect()) {
            ESP_LOGE(TAG, "Cannot upload: WiFi disconnected");
            return false;
        }
    }

    /* 执行上传（带重试） */
    int status_code = 0;
    for (int retry = 0; retry < UPLOAD_MAX_RETRIES; retry++) {
        if (retry > 0) {
            ESP_LOGW(TAG, "Sync retry %d/%d...", retry + 1, UPLOAD_MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (http_post_json(json_buf, &status_code)) {
            return true;
        }

        if (s_mod.state != WIFI_STATE_CONNECTED) {
            do_wifi_reconnect();
        }
    }

    return false;
}

bool wifi_upload_has_pending(void)
{
    if (!s_mod.upload_queue) return false;
    return uxQueueMessagesWaiting(s_mod.upload_queue) > 0;
}

void wifi_upload_reconnect(void)
{
    if (s_mod.state == WIFI_STATE_DISCONNECTED) {
        do_wifi_reconnect();
    }
}

void wifi_upload_stop(void)
{
    if (!s_mod.initialized) return;

    s_mod.keep_running = false;

    if (s_mod.upload_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_mod.upload_task_handle) {
            vTaskDelete(s_mod.upload_task_handle);
            s_mod.upload_task_handle = NULL;
        }
    }

    if (s_mod.upload_queue) {
        vQueueDelete(s_mod.upload_queue);
        s_mod.upload_queue = NULL;
    }

    if (s_mod.wifi_event_group) {
        vEventGroupDelete(s_mod.wifi_event_group);
        s_mod.wifi_event_group = NULL;
    }

    s_mod.initialized = false;
    s_mod.state = WIFI_STATE_DISCONNECTED;
    ESP_LOGI(TAG, "Stopped");
}