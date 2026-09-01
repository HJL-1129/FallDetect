#include "mpu6500.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/i2c_master.h"

#include "gsm4g.h"

static const char *TAG = "MPU6500";

#define MPU6500_ADDR             0x68
#define MPU6500_REG_WHO_AM_I     0x75
#define MPU6500_REG_PWR_MGMT_1   0x6B
#define MPU6500_REG_SMPLRT_DIV   0x19
#define MPU6500_REG_CONFIG       0x1A
#define MPU6500_REG_GYRO_CONFIG  0x1B
#define MPU6500_REG_ACCEL_CONFIG 0x1C
#define MPU6500_REG_ACCEL_XOUT_H 0x3B

#define ACCEL_SCALE_4G  8192.0f
#define GYRO_SCALE_500  65.5f

#define SAMPLE_PERIOD_MS  20
#define SAMPLE_DT         (SAMPLE_PERIOD_MS / 1000.0f)

#define FREEFALL_THRESHOLD_G   0.60f
#define IMPACT_THRESHOLD_G     2.6f
#define POSTURE_TILT_DEG       30.0f
#define FREEFALL_WINDOW_MS     500
#define IMPACT_WINDOW_MS       1000
#define POSTURE_HOLD_MS        1500
#define GYRO_FALL_THRESHOLD_DPS 180.0f

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

static mpu6500_history_t s_history;
static SemaphoreHandle_t s_history_lock;

static volatile bool s_fall_pending = false;
static mpu6500_history_t s_fall_snapshot;

static esp_err_t mpu6500_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 100);
}

static esp_err_t mpu6500_read(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, data, len, 100);
}

esp_err_t mpu6500_init(gpio_num_t sda, gpio_num_t scl)
{
    s_history_lock = xSemaphoreCreateMutex();
    if (!s_history_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_history, 0, sizeof(s_history));

    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = scl,
        .sda_io_num = sda,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MPU6500_ADDR,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c add dev: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t who = 0;
    err = mpu6500_read(MPU6500_REG_WHO_AM_I, &who, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WHO_AM_I read failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "WHO_AM_I=0x%02x", who);

    mpu6500_write_reg(MPU6500_REG_PWR_MGMT_1, 0x80);
    vTaskDelay(pdMS_TO_TICKS(100));
    mpu6500_write_reg(MPU6500_REG_PWR_MGMT_1, 0x01);
    vTaskDelay(pdMS_TO_TICKS(10));
    mpu6500_write_reg(MPU6500_REG_SMPLRT_DIV, 0x09);
    mpu6500_write_reg(MPU6500_REG_CONFIG, 0x03);
    mpu6500_write_reg(MPU6500_REG_GYRO_CONFIG, 0x08);
    mpu6500_write_reg(MPU6500_REG_ACCEL_CONFIG, 0x08);

    return ESP_OK;
}

static void push_history(float roll, float pitch, float yaw)
{
    if (xSemaphoreTake(s_history_lock, portMAX_DELAY) == pdTRUE) {
        s_history.history[s_history.head].roll  = roll;
        s_history.history[s_history.head].pitch = pitch;
        s_history.history[s_history.head].yaw   = yaw;
        s_history.head = (s_history.head + 1) % MPU6500_SAMPLE_HISTORY;
        if (s_history.head == 0) {
            s_history.full = true;
        }
        xSemaphoreGive(s_history_lock);
    }
}

static void snapshot_history(mpu6500_history_t *out)
{
    if (xSemaphoreTake(s_history_lock, portMAX_DELAY) == pdTRUE) {
        memcpy(out, &s_history, sizeof(*out));
        xSemaphoreGive(s_history_lock);
    }
}

static void mpu_task(void * arg){// 摔倒检测状态机：读MPU6500数据，检测是否摔倒

    // 第1步：定义变量
    uint8_t raw[14];// 14字节原始数据缓冲区
    float yaw_int = 0.0f;// 偏航角（Z轴角速度积分累加），初始为0
    float baseline_roll = 0;// 基准滚转角(baseline_roll)（空闲态时记录）
    float baseline_pitch = 0;// 基准俯仰角(baseline_pitch)（空闲态时记录）
    uint32_t st_enter_ms = 0;// 进入当前状态时的时刻st_enter_ms（毫秒）
    
    // 定义，枚举结构,状态：ST_IDLE = 0, ST_FREEFALL = 1, ST_IMPACT_WAIT = 2, ST_POSTURE_CHECK = 3
    int st = 0;
    enum{ST_IDLE = 0, ST_FREEFALL = 1, ST_IMPACT_WAIT = 2, ST_POSTURE_CHECK = 3};
    
    // 记录当前TickType_t数，用来做精确延时(last_wake)xtgtc
    TickType_t last_wake = xTaskGetTickCount();

    // 第2步：无限循环，每20毫秒采集一次ret
    // 从0x3B      读取     14字节原始数据（27）(14字节raw)
    // 第3步：读失败则等待后vTDU(&,pTT(SPM))重试
    while(true){
        int ret;
        ret = mpu6500_read(MPU6500_REG_ACCEL_XOUT_H,raw,14);
        if(ret!=0){
            vTaskDelayUntil(&last_wake,pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
            continue;
        }

        // ---- 第4步：拆解6轴原始数值 (高8位 << 8 | 低8位 → 合并成16位整数) ----
        int16_t ax_r = (int16_t)((raw[0] << 8) | raw[1]);
        int16_t ay_r = (int16_t)((raw[2] << 8) | raw[3]);
        int16_t az_r = (int16_t)((raw[4] << 8) | raw[5]);
        int16_t gx_r = (int16_t)((raw[6] << 8) | raw[7]);
        int16_t gy_r = (int16_t)((raw[10] << 8) | raw[11]);
        int16_t gz_r = (int16_t)((raw[12] << 8) | raw[13]);

        // ---- 第5步：原始值 ÷ 量程(ACCEL_SCALE_4G;GYRO_SCALE_500;) = 物理单位 ----
        float ax = ax_r / ACCEL_SCALE_4G;
        float ay = ay_r / ACCEL_SCALE_4G;
        float az = az_r / ACCEL_SCALE_4G;
        float gx = gx_r / GYRO_SCALE_500;
        float gy = gy_r / GYRO_SCALE_500;
        float gz = gz_r / GYRO_SCALE_500;


        // ---- 第6步：计算特征值 ----gyro_mag、svm、roll、pitch,(算法sqrtf,atan2f)
        float gyro_mag = sqrt(gx*gx+gy*gy+gz*gz);// 角速度合量（转得多剧烈）
        float svm = sqrtf(ax*ax+ay*ay+az*az);// 加速度合量（撞得多狠）
        float roll = atan2f(ay,az)*180.0f/M_PI;// 滚转角 (180/π≈57.3)
        float pitch = atan2f(-ax,sqrtf(ay*ay+az*az))*180.0f/M_PI;// 俯仰角

        // 偏航角 yaw_int gZ轴角速度*SAMPLE_DT累加,(360)
        yaw_int = yaw_int + gz * SAMPLE_DT;
        if(yaw_int > 180.0f) yaw_int = yaw_int - 360.0f;
        if(yaw_int < 180.0f) yaw_int = yaw_int + 360.0f;

        // 第7步：保存姿态历史roll, pitch, yaw_int
        push_history(roll,pitch,yaw_int);

        // 第8步：当前时间（毫秒）pTTM(xtgtc)
        uint32_t now_ms = pdTICKS_TO_MS(xTaskGetTickCount());

        // ---- 第9步：状态机 ----
        switch (st) {
        case ST_IDLE:  // 空闲态
            baseline_roll = roll;    // 记住当前没倒下的姿势
            baseline_pitch = pitch;
            // 如果转得很快（超过250°/s），可能是要摔倒了
            if (gyro_mag > GYRO_FALL_THRESHOLD_DPS) {
                if (svm < FREEFALL_THRESHOLD_G) {    // 加速度很小→自由落体
                    st = ST_FREEFALL;
                    st_enter_ms = now_ms;
                    // ESP_LOGW = 输出警告日志（看不懂看最上面①）
                    ESP_LOGW(TAG, "FREEFALL svm=%.2f gyro=%.1f", svm, gyro_mag);
                } else if (svm > IMPACT_THRESHOLD_G) {      // 加速度很大→直接撞击
                    st = ST_POSTURE_CHECK;
                    st_enter_ms = now_ms;
                    ESP_LOGW(TAG, "IMPACT svm=%.2f gyro=%.1f", svm, gyro_mag);
                }
            }
            break;

        case ST_FREEFALL:  // 自由落体态
            if (now_ms - st_enter_ms > FREEFALL_WINDOW_MS) {
                st = ST_IDLE;  // 等了500ms还没撞到→假警报
            } else if (svm > IMPACT_THRESHOLD_G) {
                st = ST_IMPACT_WAIT;  // 500ms内撞到了→撞击了
                st_enter_ms = now_ms;
                ESP_LOGW(TAG, "IMPACT after freefall svm=%.2f", svm);
            }
            break;

        case ST_IMPACT_WAIT:  // 撞击等待态
            if (now_ms - st_enter_ms > IMPACT_WINDOW_MS) {
                st = ST_POSTURE_CHECK;  // 等1000ms后检查姿势
                st_enter_ms = now_ms;
            }
            break;

        case ST_POSTURE_CHECK: {  // 姿态检查态
            float drr = fabsf(roll - baseline_roll);
            float dpp = fabsf(pitch - baseline_pitch);
            // 角度超过180°要转换一下（取较小者）
            drr = fminf(drr, 360 - drr);
            dpp = fminf(dpp, 360 - dpp);
            // 保持2500ms姿势没变回来→真摔了
            if (now_ms - st_enter_ms >= POSTURE_HOLD_MS) {
                if (drr > POSTURE_TILT_DEG || dpp > POSTURE_TILT_DEG) {
                    gsm4g_gps_t gps = {0};
                    gsm4g_get_gps(&gps);
                    ESP_LOGE(TAG, "FALL CONFIRMED dRoll=%.1f dPitch=%.1f lat=%.6f lon=%.6f",
                             drr, dpp, gps.lat, gps.lon);
                    snapshot_history(&s_fall_snapshot);
                    s_fall_pending = true;
                }
                st = ST_IDLE;  // 不管摔没摔，回到空闲态
            }
            break;
        }
        }

        // 第10步：精确等20毫秒后再采集
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
    }
}

void mpu6500_start_task(void)
{
    xTaskCreate(mpu_task, "mpu6500", 4096, NULL, 6, NULL);
}

bool mpu6500_take_fall_event(mpu6500_history_t *snapshot)
{
    if (!s_fall_pending) {
        return false;
    }
    if (snapshot) {
        memcpy(snapshot, &s_fall_snapshot, sizeof(*snapshot));
    }
    s_fall_pending = false;
    return true;
}

void mpu6500_get_history(mpu6500_history_t *out)
{
    if (!out) return;
    snapshot_history(out);
}

static void format_field(const mpu6500_history_t *h, int field_idx, char *out, size_t out_size)
{
    int count = h->full ? MPU6500_SAMPLE_HISTORY : h->head;
    if (count == 0) {
        snprintf(out, out_size, "0.000");
        return;
    }
    int start = h->full ? h->head : 0;
    out[0] = '\0';
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % MPU6500_SAMPLE_HISTORY;
        const float *v = (const float *)&h->history[idx];
        int n = snprintf(out + used, out_size - used,
                         "%s%.3f", (i == 0) ? "" : "|", v[field_idx]);
        if (n < 0 || (size_t)n >= out_size - used) break;
        used += n;
    }
}

void mpu6500_format_history(const mpu6500_history_t *h,
                            char *roll_str, size_t roll_size,
                            char *pitch_str, size_t pitch_size,
                            char *yaw_str, size_t yaw_size)
{
    format_field(h, 0, roll_str,  roll_size);
    format_field(h, 1, pitch_str, pitch_size);
    format_field(h, 2, yaw_str,   yaw_size);
}
