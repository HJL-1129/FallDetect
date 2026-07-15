/**
 * @file fall_data.h
 * @brief 跌倒检测数据统一格式定义
 * 
 * 兼容现有 main.c 中的数据字段，新增标准字段:
 * - timestamp: 事件发生时间
 * - angle: 倾斜角（由Roll/Pitch计算）
 * - acceleration: 合成加速度（SVM）
 * - coordinate: 坐标系（固定WGS84）
 */

#ifndef FALL_DATA_H
#define FALL_DATA_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 设备ID最大长度 */
#define DEVICE_ID_MAX_LEN        32
/* 时间戳字符串最大长度 */
#define TIMESTAMP_STR_MAX_LEN    32
/* JSON缓冲区大小 */
#define JSON_BUF_SIZE            512

/**
 * @brief 跌倒检测事件数据结构
 * 
 * 上传至服务器的标准JSON格式对应字段:
 * {
 *   "device_id":   "fall_device_001",      // 设备编号
 *   "event":       "fall",                  // 事件类型
 *   "timestamp":   "2026-07-10 12:30:00",  // 发生时间
 *   "latitude":    25.033964,              // 纬度 (WGS84)
 *   "longitude":   121.564468,             // 经度 (WGS84)
 *   "coordinate":  "WGS84",                // 坐标系
 *   "angle":       75.0,                   // 倾斜角 (度)
 *   "acceleration": 3.5                    // 加速度 (g)
 * }
 */
typedef struct {
    char     device_id[DEVICE_ID_MAX_LEN];       /* 设备编号 */
    char     event[16];                           /* 事件类型: "fall" */
    char     timestamp[TIMESTAMP_STR_MAX_LEN];    /* ISO格式时间戳 */
    double   latitude;                            /* 纬度 (WGS84) */
    double   longitude;                           /* 经度 (WGS84) */
    char     coordinate[8];                       /* 坐标系: "WGS84" */
    float    angle;                               /* 倾斜角 (度) */
    float    acceleration;                        /* 合成加速度 (g) */
    /* 以下为兼容现有代码的扩展字段 */
    uint8_t  btn;                                 /* 触发按键(0=跌倒/无按键) */
    uint8_t  state;                               /* 状态码(1=跌倒) */
    uint16_t battery_mv;                          /* 电池电压(mV) */
} fall_event_t;

/**
 * @brief 将跌倒事件结构体序列化为JSON字符串
 * 
 * @param event  跌倒事件结构体指针
 * @param buf    输出缓冲区
 * @param buf_size 缓冲区大小
 * @return int   成功返回写入字节数，失败返回-1
 */
int fall_event_to_json(const fall_event_t *event, char *buf, size_t buf_size);

/**
 * @brief 初始化跌倒事件结构体（设置默认值）
 * 
 * @param event 跌倒事件结构体指针
 * @param device_id 设备ID
 */
void fall_event_init(fall_event_t *event, const char *device_id);

/**
 * @brief 设置时间戳为当前时间
 * 
 * @param event 跌倒事件结构体指针
 */
void fall_event_set_timestamp_now(fall_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* FALL_DATA_H */