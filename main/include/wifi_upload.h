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
 * ⚠️ 部署到 Cloudflare Workers 后请修改 UPLOAD_SERVER_URL 为你的 Worker 地址
 * 例如: https://fall-api.你的用户名.workers.dev
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
 * ⚠️ 重要: 部署Cloudflare Worker后，必须修改下面3个宏为你的Worker地址
 * 
 * 示例:
 *   Step 1: 在 Cloudflare 控制台创建 Worker (名称: fall-api)
 *   Step 2: 部署后获得地址如: https://fall-api.xxx.workers.dev
 *   Step 3: 将下面的 URL 改为该地址
 * ============================================================ */

/* 你的 Cloudflare Worker 地址（部署后修改这里！） */
// 改为 Worker 地址
#define UPLOAD_SERVER_URL       "https://api.lele1129.top"
#define UPLOAD_SERVER_PATH      "/api/fall"
#define UPLOAD_SERVER_PORT      443

/* 完整API URL（自动拼接，一般无需修改） */
#define LELE1129_API_URL        UPLOAD_SERVER_URL UPLOAD_SERVER_PATH           /* HTTPS（WiFi上传使用） */
#define LELE1129_API_URL_HTTP   "http://lele1129.top/api/fall"                 /* HTTP备用（4G模块AT指令） */

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