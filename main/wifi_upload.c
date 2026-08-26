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
 * 
 * 修复记录 (2026-07-15):
 * - 添加互斥锁保护 s_mod.state 并发访问（WiFi事件回调 vs 上传任务）
 * - 修复事件处理器句柄泄漏（存储 unregister 用的句柄）
 * - 消除HTTP回退代码重复（提取 http_post_to_url 函数）
 * - 修复 shutdown 安全（通过队列通知任务自然退出）
 * - 队列容量从8扩展到16，减少丢事件概率
 */

#include "wifi_upload.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#define TAG "WIFI_UPLOAD"

/* 当HTTPS失败时是否允许自动回退到HTTP（本地测试建议关闭回退） */
/* 2026-08-24: 开启回退。Cloudflare 已放行 http://lele1129.top/api/fall（返回200，
 * 不再301跳转），HTTPS万一还是失败时自动用HTTP补传，保证跌倒数据必达服务器。 */
#define UPLOAD_HTTP_FALLBACK_ENABLED  true

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
static SemaphoreHandle_t s_mod_lock = NULL;          /* 保护 s_mod.state 互斥 */

static struct {
    bool               initialized;           /* 是否已初始化 */
    EventGroupHandle_t wifi_event_group;       /* WiFi事件组 */
    wifi_state_t       state;                 /* 当前WiFi状态 */
    QueueHandle_t      upload_queue;           /* 上传队列 */
    TaskHandle_t       upload_task_handle;     /* 上传任务句柄 */
    uint32_t           reconnect_interval_ms;  /* 重连间隔(ms) */
    bool               keep_running;           /* 运行标志 */
    /* 事件处理器句柄，用于 shutdown 时反注册 */
    esp_event_handler_instance_t wifi_any_id;
    esp_event_handler_instance_t ip_got_ip_id;
} s_mod = {
    .initialized          = false,
    .wifi_event_group     = NULL,
    .state                = WIFI_STATE_DISCONNECTED,
    .upload_queue         = NULL,
    .upload_task_handle   = NULL,
    .reconnect_interval_ms = WIFI_RECONNECT_MS,
    .keep_running         = false,
    .wifi_any_id          = NULL,
    .ip_got_ip_id         = NULL,
};

/* ============================ 内部辅助函数 ============================ */

static wifi_state_t get_wifi_state(void)
{
    wifi_state_t st = WIFI_STATE_DISCONNECTED;
    if (xSemaphoreTake(s_mod_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        st = s_mod.state;
        xSemaphoreGive(s_mod_lock);
    }
    return st;
}

static void set_wifi_state(wifi_state_t st)
{
    if (xSemaphoreTake(s_mod_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_mod.state = st;
        xSemaphoreGive(s_mod_lock);
    }
}

/* ============================ WiFi事件处理 ============================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected");
        set_wifi_state(WIFI_STATE_DISCONNECTED);
        if (s_mod.wifi_event_group) {
            xEventGroupSetBits(s_mod.wifi_event_group, WIFI_DISCONNECT_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        set_wifi_state(WIFI_STATE_CONNECTED);
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
 * @brief 执行单次HTTP POST上传（可指定URL）
 * 
 * @param url        完整URL字符串
 * @param json_buf   JSON数据字符串
 * @param status_code 输出HTTP状态码
 * @return true 上传成功, false 上传失败
 */
static bool http_post_to_url(const char *url, const char *json_buf, int *status_code)
{
    if (!url || !json_buf || !status_code) return false;

    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = http_event_handler,
        .timeout_ms    = 10000,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach = esp_crt_bundle_attach,  /* 挂载内置CA证书包（修复HTTPS握手失败） */
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init HTTP client for %s", url);
        *status_code = 0;
        return false;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_buf, strlen(json_buf));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    *status_code = status;

    bool success = (err == ESP_OK && status >= 200 && status < 300);

    if (success) {
        ESP_LOGI(TAG, "POST %s OK, HTTP %d", url, status);
    } else {
        ESP_LOGW(TAG, "POST %s FAIL: err=%s, HTTP %d", url,
                 esp_err_to_name(err), status);
    }

    esp_http_client_cleanup(client);
    return success;
}

/**
 * @brief 执行一次HTTP POST上传（使用配置的URL）
 */
static bool http_post_json(const char *json_buf, int *status_code)
{
    /* 使用头文件中配置的 UPLOAD_API_URL */
    return http_post_to_url(UPLOAD_API_URL, json_buf, status_code);
}

/**
 * @brief 执行一次WiFi重连
 */
static bool do_wifi_reconnect(void)
{
    if (get_wifi_state() == WIFI_STATE_CONNECTING) {
        return false;
    }

    set_wifi_state(WIFI_STATE_CONNECTING);

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        set_wifi_state(WIFI_STATE_ERROR);
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_mod.wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_DISCONNECT_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(10000)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        set_wifi_state(WIFI_STATE_CONNECTED);
        return true;
    }

    ESP_LOGW(TAG, "Reconnect timeout");
    set_wifi_state(WIFI_STATE_DISCONNECTED);
    return false;
}

/* ============================ 上传重试逻辑 ============================ */

/**
 * @brief 尝试上传，HTTPS优先，失败则HTTP回退
 * 
 * @return true 上传成功
 */
static bool upload_with_retry(const char *json_buf)
{
    int status_code = 0;

    /* 第一次尝试：HTTPS上传 */
    for (int retry = 0; retry < UPLOAD_MAX_RETRIES; retry++) {
        if (retry > 0) {
            ESP_LOGW(TAG, "HTTPS retry %d/%d...", retry + 1, UPLOAD_MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(1000 * retry));
        }

        if (http_post_json(json_buf, &status_code)) {
            return true;
        }

        if (get_wifi_state() != WIFI_STATE_CONNECTED) {
            do_wifi_reconnect();
        }
    }

    /* HTTPS全部失败 → HTTP回退（只对云端模式有效） */
    if (!UPLOAD_HTTP_FALLBACK_ENABLED) {
        return false;
    }

    ESP_LOGW(TAG, "HTTPS failed, trying HTTP fallback...");

    /* 尝试从HTTPS URL推导HTTP回退URL */
    char http_url[512];
    const char *https_url = UPLOAD_API_URL;
    if (strncmp(https_url, "https://", 8) == 0) {
        snprintf(http_url, sizeof(http_url), "http://%s", https_url + 8);
    } else {
        snprintf(http_url, sizeof(http_url), "%s", https_url);
    }

    for (int retry = 0; retry < UPLOAD_MAX_RETRIES; retry++) {
        if (retry > 0) {
            ESP_LOGW(TAG, "HTTP retry %d/%d...", retry + 1, UPLOAD_MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(1000 * retry));
        }

        if (http_post_to_url(http_url, json_buf, &status_code)) {
            return true;
        }

        if (get_wifi_state() != WIFI_STATE_CONNECTED) {
            do_wifi_reconnect();
        }
    }

    return false;
}

/* ============================ 上传任务 ============================ */

static void upload_task_func(void *arg)
{
    (void)arg;
    upload_queue_item_t item;

    ESP_LOGI(TAG, "Upload task started");

    while (s_mod.keep_running) {
        BaseType_t received = xQueueReceive(
            s_mod.upload_queue, &item, pdMS_TO_TICKS(1000));

        if (received != pdTRUE) {
            continue;
        }

        /* 检查WiFi状态，断线则等待重连 */
        while (get_wifi_state() != WIFI_STATE_CONNECTED && s_mod.keep_running) {
            ESP_LOGW(TAG, "WiFi not connected, reconnecting...");
            do_wifi_reconnect();

            if (get_wifi_state() != WIFI_STATE_CONNECTED) {
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

        bool success = upload_with_retry(json_buf);

        if (success) {
            ESP_LOGI(TAG, "Upload success");
        } else {
            ESP_LOGE(TAG, "Upload failed after retries");
        }

        if (item.callback) {
            item.callback(success, 0, item.user_data);
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

    (void)ssid;
    (void)password;

    /* 创建互斥锁 */
    s_mod_lock = xSemaphoreCreateMutex();
    if (!s_mod_lock) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return false;
    }

    /* 创建WiFi事件组 */
    s_mod.wifi_event_group = xEventGroupCreate();
    if (!s_mod.wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        vSemaphoreDelete(s_mod_lock);
        s_mod_lock = NULL;
        return false;
    }

    /* 注册事件处理器 */
    s_mod.wifi_any_id = NULL;
    s_mod.ip_got_ip_id = NULL;

    esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID,
        &wifi_event_handler, NULL, &s_mod.wifi_any_id);
    esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP,
        &wifi_event_handler, NULL, &s_mod.ip_got_ip_id);

    /* 创建上传队列（最大16项） */
    s_mod.upload_queue = xQueueCreate(16, sizeof(upload_queue_item_t));
    if (!s_mod.upload_queue) {
        ESP_LOGE(TAG, "Failed to create upload queue");
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_mod.wifi_any_id);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_mod.ip_got_ip_id);
        vEventGroupDelete(s_mod.wifi_event_group);
        s_mod.wifi_event_group = NULL;
        vSemaphoreDelete(s_mod_lock);
        s_mod_lock = NULL;
        return false;
    }

    /* 检查当前WiFi状态 */
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        set_wifi_state(WIFI_STATE_CONNECTED);
        ESP_LOGI(TAG, "WiFi already connected to %s", ap_info.ssid);
        xEventGroupSetBits(s_mod.wifi_event_group, WIFI_CONNECTED_BIT);
    } else {
        set_wifi_state(WIFI_STATE_DISCONNECTED);
        ESP_LOGW(TAG, "WiFi not connected, will reconnect when needed");
    }

    /* 创建上传任务 */
    s_mod.keep_running = true;
    BaseType_t task_created = xTaskCreate(
        upload_task_func,
        "wifi_upload",
        4096,
        NULL,
        4,
        &s_mod.upload_task_handle
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create upload task");
        vQueueDelete(s_mod.upload_queue);
        s_mod.upload_queue = NULL;
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_mod.wifi_any_id);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_mod.ip_got_ip_id);
        vEventGroupDelete(s_mod.wifi_event_group);
        s_mod.wifi_event_group = NULL;
        vSemaphoreDelete(s_mod_lock);
        s_mod_lock = NULL;
        return false;
    }

    s_mod.initialized = true;
    ESP_LOGI(TAG, "Initialized");
    return true;
}

wifi_state_t wifi_upload_get_state(void)
{
    return get_wifi_state();
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

    char json_buf[JSON_BUF_SIZE];
    int len = fall_event_to_json(event, json_buf, sizeof(json_buf));
    if (len < 0) {
        ESP_LOGE(TAG, "JSON serialize failed");
        return false;
    }

    if (get_wifi_state() != WIFI_STATE_CONNECTED) {
        ESP_LOGW(TAG, "WiFi not connected, attempting reconnect...");
        if (!do_wifi_reconnect()) {
            ESP_LOGE(TAG, "Cannot upload: WiFi disconnected");
            return false;
        }
    }

    return upload_with_retry(json_buf);
}

bool wifi_upload_has_pending(void)
{
    if (!s_mod.upload_queue) return false;
    return uxQueueMessagesWaiting(s_mod.upload_queue) > 0;
}

void wifi_upload_reconnect(void)
{
    if (get_wifi_state() == WIFI_STATE_DISCONNECTED) {
        do_wifi_reconnect();
    }
}

void wifi_upload_stop(void)
{
    if (!s_mod.initialized) return;

    s_mod.keep_running = false;

    if (s_mod.upload_task_handle) {
        /* 发空事件唤醒任务并退出 */
        upload_queue_item_t dummy;
        memset(&dummy, 0, sizeof(dummy));
        xQueueSend(s_mod.upload_queue, &dummy, pdMS_TO_TICKS(100));
        vTaskDelay(pdMS_TO_TICKS(200));
        if (s_mod.upload_task_handle) {
            vTaskDelete(s_mod.upload_task_handle);
            s_mod.upload_task_handle = NULL;
        }
    }

    if (s_mod.upload_queue) {
        vQueueDelete(s_mod.upload_queue);
        s_mod.upload_queue = NULL;
    }

    /* 反注册事件处理器 */
    if (s_mod.wifi_any_id) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_mod.wifi_any_id);
        s_mod.wifi_any_id = NULL;
    }
    if (s_mod.ip_got_ip_id) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_mod.ip_got_ip_id);
        s_mod.ip_got_ip_id = NULL;
    }

    if (s_mod.wifi_event_group) {
        vEventGroupDelete(s_mod.wifi_event_group);
        s_mod.wifi_event_group = NULL;
    }

    if (s_mod_lock) {
        vSemaphoreDelete(s_mod_lock);
        s_mod_lock = NULL;
    }

    s_mod.initialized = false;
    set_wifi_state(WIFI_STATE_DISCONNECTED);
    ESP_LOGI(TAG, "Stopped");
}