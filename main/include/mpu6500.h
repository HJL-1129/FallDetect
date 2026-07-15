#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MPU6500_SAMPLE_HISTORY 5

typedef struct {
    float roll;
    float pitch;
    float yaw;
} mpu6500_angle_t;

typedef struct {
    mpu6500_angle_t history[MPU6500_SAMPLE_HISTORY];
    uint8_t head;
    bool full;
} mpu6500_history_t;

esp_err_t mpu6500_init(gpio_num_t sda, gpio_num_t scl);

void mpu6500_start_task(void);

bool mpu6500_take_fall_event(mpu6500_history_t *snapshot);

void mpu6500_get_history(mpu6500_history_t *out);

void mpu6500_format_history(const mpu6500_history_t *h,
                            char *roll_str, size_t roll_size,
                            char *pitch_str, size_t pitch_size,
                            char *yaw_str, size_t yaw_size);

#ifdef __cplusplus
}
#endif
