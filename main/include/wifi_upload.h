/**
 * @file wifi_upload.h
 * @brief WiFi HTTP上传模块
 * 
 * 负责通过WiFi将跌倒事件数据以HTTP POST方式上传至Cloudflare Worker API。
 * 特性:
 * - WiFi断线自动重连
 * - 上传失败自动重试（最多3次）
 * - 网络异常保护（超时控制）
 * - 非阻塞设计（FreeRTOS任务+队列）
 * 
 * 两种使用模式切换:
 *   模式A - 本地测试: 电脑运行 server/server_local.js，ESP32上传到局域网
 *   模式B - 云端部署: 部署 worker.js 到 Cloudflare Workers
 * 
 * Content-Type: application/json
 */

#ifndef WIFI_UPLOAD_H
#define WIFI_UPLOAD_H

#include <stdint.h>
#include <stdbool.h>
#include "fall_data.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 上传服务器配置
 * ============================================================
 * 
 * 两种使用模式切换（注释/取消注释即可）:
 * 
 * 模式A - 本地测试（ESP32 和电脑连同一个 WiFi）:
 *   1. 在电脑上运行: node server/server_local.js
 *   2. 取消注释下方的 LOCAL_TEST 配置
 *   3. 编译烧录 ESP32
 *   4. 浏览器打开 http://电脑IP:3000 看 GIS 地图
 * 
 * 模式B - 云端部署（Cloudflare Workers）:
 *   1. 注册 Cloudflare 账号
 *   2. 部署: cd server && wrangler deploy worker.js --name fall-api
 *   3. 获得地址如: https://fall-api.xxx.workers.dev
 *   4. 取消注释下方的 CLOUD_DEPLOY 配置并修改 URL
 * 
 * ========== ⚠️ 4G 模块上传关键警告 ==========
 * 
 * 4G 模块（Quectel AT+QHTTP）只支持 HTTP 明文上传，且【不支持 HTTPS、不跟随重定向】。
 * 
 * Cloudflare 默认开启 "Always Use HTTPS"：
 *   http://lele1129.top/api/fall  →  301  →  https://lele1129.top/api/fall
 * 
 * 4G 模块收到 301 后不会继续请求 https，导致上传必然失败（服务器收不到数据，
 * GIS 地图无坐标）。若你的设备主要走 4G 网络，必须做【二选一】：
 * 
 *   方案1（推荐）: 在 Cloudflare 控制台 → SSL/TLS → Edge Certificates →
 *                 关闭 "Always Use HTTPS"，同时添加一条规则允许 /api/fall 走 HTTP。
 *   方案2: 4G 模块换用支持 TLS 的 AT 指令（如 EC200 的 AT+QHTTP 不支持，需要
 *                 改用 AT+QSSL 或换支持 SSL 的模块/固件），成本较高。
 * 
 * ESP32 的 WiFi 路径（esp_http_client）支持 HTTPS，不受影响。
 * ============================================================ */

/* ========== 模式选择：二选一取消注释 ========== */

// --- 模式A：本地测试 ---
// #define UPLOAD_SERVER_URL       "http://192.168.3.31"   // ← 修改为你的电脑局域网IP
// #define UPLOAD_SERVER_PATH      ":3000/api/fall"         // 本地服务器端口+路径
// #define UPLOAD_SERVER_PORT      3000
// #define UPLOAD_API_URL          UPLOAD_SERVER_URL UPLOAD_SERVER_PATH

// --- 模式B：云端部署（Cloudflare Workers） ---
#define UPLOAD_SERVER_URL       "https://lele1129.top"
#define UPLOAD_SERVER_PATH      "/api/fall"
#define UPLOAD_SERVER_PORT      443
#define UPLOAD_API_URL          UPLOAD_SERVER_URL UPLOAD_SERVER_PATH

/* ============================================================ */

/* HTTP备用（4G模块AT指令可能不支持HTTPS） */
#define LELE1129_API_URL_HTTP   "http://lele1129.top/api/fall"

/* 重试次数 */
#define UPLOAD_MAX_RETRIES      3

/* WiFi重连间隔(ms) */
#define WIFI_RECONNECT_MS       3000

/**
 * @brief WiFi状态枚举
 */
typedef enum {
    WIFI_STATE_DISCONNECTED = 0,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_ERROR
} wifi_state_t;

/**
 * @brief 上传结果回调
 * 
 * @param success true=上传成功, false=上传失败
 * @param status_code HTTP状态码(上传失败时为0)
 * @param user_data 用户自定义数据(可传NULL)
 */
typedef void (*upload_callback_t)(bool success, int status_code, void *user_data);

/**
 * @brief 初始化WiFi并连接到保存的AP
 * 
 * @param ssid      WiFi SSID (NULL则从NVS读取)
 * @param password  WiFi密码 (NULL则从NVS读取)
 * @return true 初始化成功, false 初始化失败
 */
bool wifi_upload_init(const char *ssid, const char *password);

/**
 * @brief 获取当前WiFi连接状态
 * 
 * @return wifi_state_t 当前状态
 */
wifi_state_t wifi_upload_get_state(void);

/**
 * @brief 异步上传跌倒事件数据
 * 
 * 将数据放入队列，由后台任务处理上传。
 * 此函数立即返回，不阻塞调用者。
 * 
 * @param event    跌倒事件数据指针（内部会拷贝，调用者可释放）
 * @param callback 上传完成回调（可为NULL）
 * @param user_data 回调用户数据（可为NULL）
 * @return true 提交成功, false 队列满提交失败
 */
bool wifi_upload_event_async(const fall_event_t *event,
                              upload_callback_t callback,
                              void *user_data);

/**
 * @brief 同步上传跌倒事件数据（阻塞方式）
 * 
 * 注意：不要在main loop或中断中调用，会阻塞当前任务。
 * 推荐在主循环中调用此函数，但确保非频繁调用。
 * 
 * @param event 跌倒事件数据指针
 * @return true 上传成功, false 上传失败
 */
bool wifi_upload_event_sync(const fall_event_t *event);

/**
 * @brief 检查是否有待上传的数据
 * 
 * @return true 队列中有待上传数据
 */
bool wifi_upload_has_pending(void);

/**
 * @brief 手动触发WiFi重连
 */
void wifi_upload_reconnect(void);

/**
 * @brief 停止上传模块（释放资源）
 */
void wifi_upload_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_UPLOAD_H */