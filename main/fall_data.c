/**
 * @file fall_data.c
 * @brief 跌倒检测数据格式实现
 * 
 * 提供JSON序列化和时间戳设置功能。
 * 使用标准C库，不依赖外部JSON库，手写JSON序列化保持轻量。
 */

#include "fall_data.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

int fall_event_to_json(const fall_event_t *event, char *buf, size_t buf_size)
{
    /* 最小缓冲区至少等于 JSON_BUF_SIZE(512) 以保证完整输出 */
    if (!event || !buf || buf_size < JSON_BUF_SIZE) {
        return -1;
    }

    /*
     * 最后防线：任何来源的 NaN/Inf 都可能导致 JSON 非法（如 "angle":nan），
     * 从而使服务器解析失败（HTTP 400）。这里在序列化前统一规约为合法数值。
     */
    double angle        = (double)event->angle;
    double acceleration = (double)event->acceleration;
    double latitude     = event->latitude;
    double longitude    = event->longitude;
    if (!isfinite(angle))        angle        = 0.0;
    if (!isfinite(acceleration)) acceleration = 0.0;
    if (!isfinite(latitude))     latitude     = 0.0;
    if (!isfinite(longitude))    longitude    = 0.0;

    int written = snprintf(buf, buf_size,
        "{"
        "\"device_id\":\"%s\","
        "\"event\":\"%s\","
        "\"timestamp\":\"%s\","
        "\"latitude\":%.6f,"
        "\"longitude\":%.6f,"
        "\"coordinate\":\"%s\","
        "\"angle\":%.1f,"
        "\"acceleration\":%.1f,"
        "\"btn\":%u,"
        "\"state\":%u,"
        "\"battery_mv\":%u"
        "}",
        event->device_id,
        event->event,
        event->timestamp,
        latitude,
        longitude,
        event->coordinate,
        angle,
        acceleration,
        (unsigned int)event->btn,
        (unsigned int)event->state,
        (unsigned int)event->battery_mv
    );

    if (written < 0 || (size_t)written >= buf_size) {
        return -1;
    }

    return written;
}

void fall_event_init(fall_event_t *event, const char *device_id)
{
    if (!event) return;

    memset(event, 0, sizeof(fall_event_t));

    /* 设置设备ID */
    if (device_id) {
        strncpy(event->device_id, device_id, DEVICE_ID_MAX_LEN - 1);
        event->device_id[DEVICE_ID_MAX_LEN - 1] = '\0';
    }

    /* 默认事件类型 */
    strncpy(event->event, "fall", sizeof(event->event) - 1);
    event->event[sizeof(event->event) - 1] = '\0';

    /* 默认坐标系 */
    strncpy(event->coordinate, "WGS84", sizeof(event->coordinate) - 1);
    event->coordinate[sizeof(event->coordinate) - 1] = '\0';

    /* 设置默认时间戳 */
    fall_event_set_timestamp_now(event);
}

void fall_event_set_timestamp_now(fall_event_t *event)
{
    if (!event) return;

    /* 使用time(NULL)获取Unix时间戳，转换为本地时间 */
    time_t now;
    struct tm *timeinfo;

    time(&now);
    timeinfo = localtime(&now);

    if (timeinfo) {
        strftime(event->timestamp, TIMESTAMP_STR_MAX_LEN,
                 "%Y-%m-%d %H:%M:%S", timeinfo);
    } else {
        strncpy(event->timestamp, "1970-01-01 00:00:00", TIMESTAMP_STR_MAX_LEN - 1);
        event->timestamp[TIMESTAMP_STR_MAX_LEN - 1] = '\0';
    }
}