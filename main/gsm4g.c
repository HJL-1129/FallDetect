#include "gsm4g.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "GSM4G";

#define UART_BUF_SIZE   2048
#define LINE_BUF_SIZE   512


static int s_uart_num = -1;
static SemaphoreHandle_t s_lock;
static volatile bool s_ready = false;
static char s_imei[20] = {0};

static gsm4g_gps_t s_last_gps;
static SemaphoreHandle_t s_gps_lock;

static void gsm4g_send_raw(const char *data, size_t len)
{
    uart_write_bytes(s_uart_num, data, len);
}

static void gsm4g_send_line(const char *cmd)
{
    uart_flush_input(s_uart_num);
    gsm4g_send_raw(cmd, strlen(cmd));
    gsm4g_send_raw("\r\n", 2);
}

static int read_line(char *line, size_t max, uint32_t timeout_ms)
{
    size_t pos = 0;
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS < timeout_ms) {
        uint8_t c;
        int n = uart_read_bytes(s_uart_num, &c, 1, pdMS_TO_TICKS(50));
        if (n <= 0) continue;
        if (c == '\r') continue;
        if (c == '\n') {
            if (pos > 0) {
                line[pos] = '\0';
                return (int)pos;
            }
            continue;
        }
        if (pos < max - 1) {
            line[pos++] = c;
        }
    }
    line[pos] = '\0';
    return (pos > 0) ? (int)pos : -1;
}

static esp_err_t wait_token(const char *expected, char *capture, size_t cap_size, uint32_t timeout_ms)
{
    char line[LINE_BUF_SIZE];
    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS < timeout_ms) {
        uint32_t remain = timeout_ms - (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
        int n = read_line(line, sizeof(line), remain > 0 ? remain : 1);
        if (n <= 0) continue;
        ESP_LOGI(TAG, "<- %s", line);
        if (capture && cap_size > 0) {
            strncpy(capture, line, cap_size - 1);
            capture[cap_size - 1] = '\0';
        }
        if (strstr(line, expected)) return ESP_OK;
        if (strstr(line, "ERROR")) return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t at_send(const char *cmd, uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "-> %s", cmd);
    gsm4g_send_line(cmd);
    return wait_token("OK", NULL, 0, timeout_ms);
}

static esp_err_t at_send_capture(const char *cmd, const char *match, char *out, size_t out_size, uint32_t timeout_ms)
{
    ESP_LOGI(TAG, "-> %s", cmd);
    gsm4g_send_line(cmd);
    char line[LINE_BUF_SIZE];
    TickType_t start = xTaskGetTickCount();
    bool got = false;
    while ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS < timeout_ms) {
        uint32_t remain = timeout_ms - (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
        int n = read_line(line, sizeof(line), remain > 0 ? remain : 1);
        if (n <= 0) continue;
        ESP_LOGI(TAG, "<- %s", line);
        if (!got && strstr(line, match)) {
            strncpy(out, line, out_size - 1);
            out[out_size - 1] = '\0';
            got = true;
        }
        if (strstr(line, "OK")) return got ? ESP_OK : ESP_FAIL;
        if (strstr(line, "ERROR")) return ESP_FAIL;
    }
    return ESP_ERR_TIMEOUT;
}

static void hw_power_on(gpio_num_t en, gpio_num_t rst)
{
    /* Per SCH: RESET_4G drives an NMOS whose drain ties to both PWRKEY
     * (through R50) and RESET_N. Pulling RESET_4G HIGH pulls BOTH lines
     * low simultaneously; LOW releases them. So the boot pulse is:
     *   idle (LOW) -> press (HIGH ~750ms) -> release (LOW) -> wait boot.
     */
    if (en != GPIO_NUM_NC) {
        gpio_reset_pin(en);
        gpio_set_direction(en, GPIO_MODE_OUTPUT);
        gpio_set_level(en, 1);              /* enable VCC_4G */
        vTaskDelay(pdMS_TO_TICKS(100));     /* let VCC_4G settle */
    }
    if (rst != GPIO_NUM_NC) {
        gpio_reset_pin(rst);
        gpio_set_direction(rst, GPIO_MODE_OUTPUT);
        gpio_set_level(rst, 0);             /* idle: PWRKEY/RESET released */
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level(rst, 1);             /* press PWRKEY (and reset) */
        vTaskDelay(pdMS_TO_TICKS(750));     /* Quectel needs >=500ms */
        gpio_set_level(rst, 0);             /* release */
    }
    vTaskDelay(pdMS_TO_TICKS(5000));        /* module boot time */
}

static void gsm4g_task(void *arg)
{
    char line[LINE_BUF_SIZE];

    for (int i = 0; i < 30; i++) {
        if (at_send("AT", 1000) == ESP_OK) {
            ESP_LOGI(TAG, "module responding");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    at_send("ATE0", 1000);
    at_send("AT+CMEE=2", 1000);

    if (at_send_capture("AT+CGSN", "", s_imei, sizeof(s_imei), 2000) == ESP_OK) {
        for (size_t i = 0; i < strlen(s_imei); i++) {
            if (!isdigit((unsigned char)s_imei[i])) { s_imei[i] = '\0'; break; }
        }
        ESP_LOGI(TAG, "IMEI=%s", s_imei);
    }

    bool registered = false;
    for (int i = 0; i < 60; i++) {
        if (at_send_capture("AT+CREG?", "+CREG:", line, sizeof(line), 2000) == ESP_OK) {
            int n = 0, stat = 0;
            if (sscanf(line, "+CREG: %d,%d", &n, &stat) >= 2 && (stat == 1 || stat == 5)) {
                registered = true;
                ESP_LOGI(TAG, "CS registered stat=%d", stat);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    if (!registered) {
        ESP_LOGW(TAG, "Network not registered (continuing anyway)");
    }

    at_send("AT+QICSGP=1,1,\"CMNET\",\"\",\"\",1", 2000);
    at_send("AT+QIACT=1", 30000);
    at_send("AT+QHTTPCFG=\"contextid\",1", 2000);
    at_send("AT+QHTTPCFG=\"responseheader\",0", 2000);

    at_send("AT+QGPS=1", 5000);

    s_ready = true;
    ESP_LOGI(TAG, "init done, ready=true");

    while (true) {
        /* 拿 UART 互斥锁，避免与 gsm4g_http_post_json 并发操作 AT 指令 */
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(3000)) == pdTRUE) {
            if (at_send_capture("AT+QGPSLOC=2", "+QGPSLOC:", line, sizeof(line), 5000) == ESP_OK) {
                char *p = strchr(line, ':');
                if (p) {
                    p++;
                    char *tok = strtok(p, ",");
                    int idx = 0;
                    double lat = 0, lon = 0;
                    while (tok) {
                        while (*tok == ' ') tok++;
                        if (idx == 1) lat = atof(tok);
                        else if (idx == 2) lon = atof(tok);
                        tok = strtok(NULL, ",");
                        idx++;
                    }
                    if (lat != 0 && lon != 0) {
                        if (xSemaphoreTake(s_gps_lock, portMAX_DELAY) == pdTRUE) {
                            s_last_gps.valid = true;
                            s_last_gps.lat = lat;
                            s_last_gps.lon = lon;
                            xSemaphoreGive(s_gps_lock);
                        }
                        ESP_LOGI(TAG, "GPS: lat=%.6f lon=%.6f", lat, lon);
                    }
                }
            }
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

esp_err_t gsm4g_init(int uart_num, gpio_num_t tx, gpio_num_t rx,
                     gpio_num_t pwr_en, gpio_num_t reset_n)
{
    // 保存UART端口号到全局变量
    s_uart_num = uart_num;
    // 创建互斥锁，保护AT指令串行化访问
    s_lock = xSemaphoreCreateMutex();
    // 创建GPS数据互斥锁
    s_gps_lock = xSemaphoreCreateMutex();
    // 如果任一互斥锁创建失败，返回内存不足错误
    if (!s_lock || !s_gps_lock) return ESP_ERR_NO_MEM;
    // UART通信参数配置：波特率115200、8数据位、无校验、1停止位、无流控
    uart_config_t cfg = {
        // 波特率115200
        .baud_rate = 115200,
        // 8位数据位
        .data_bits = UART_DATA_8_BITS,
        // 无校验位
        .parity = UART_PARITY_DISABLE,
        // 1位停止位
        .stop_bits = UART_STOP_BITS_1,
        // 无硬件流控
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        // 使用默认时钟源
        .source_clk = UART_SCLK_DEFAULT,
    };
    // 安装UART驱动，接收缓冲区2048*2字节
    ESP_ERROR_CHECK(uart_driver_install(uart_num, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    // 应用UART参数配置
    ESP_ERROR_CHECK(uart_param_config(uart_num, &cfg));
    // 配置UART的TX/RX引脚（RTS/CTS不改变）
    ESP_ERROR_CHECK(uart_set_pin(uart_num, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // 执行4G模块硬件上电时序（使能电源+PWRKEY脉冲）
    hw_power_on(pwr_en, reset_n);
    // 创建4G后台任务"gsm4g"：栈4096字节、优先级5
    xTaskCreate(gsm4g_task, "gsm4g", 4096, NULL, 5, NULL);
    // 返回初始化成功
    return ESP_OK;
}

bool gsm4g_is_ready(void) { return s_ready; }

bool gsm4g_get_gps(gsm4g_gps_t *out)
{
    if (!out) return false;
    if (!s_gps_lock) return false;

    bool ok = false;

    if (xSemaphoreTake(s_gps_lock, portMAX_DELAY) == pdTRUE) {
        *out = s_last_gps;
        ok = s_last_gps.valid;
        xSemaphoreGive(s_gps_lock);
    }
    return ok;
}

void gsm4g_get_imei(char *out, size_t len)
{
    if (!out || len == 0) return;
    strncpy(out, s_imei, len - 1);
    out[len - 1] = '\0';
}

static void gsm4g_gps_only_task(void *arg)
{
    char line[LINE_BUF_SIZE];

    /* 等待模块 AT 响应（最多 30 秒） */
    bool at_ok = false;
    for (int i = 0; i < 30; i++) {
        if (at_send("AT", 1000) == ESP_OK) {
            at_ok = true;
            ESP_LOGI(TAG, "module responding (GPS-only)");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!at_ok) {
        ESP_LOGE(TAG, "module not responding, GPS will not work");
        vTaskDelete(NULL);
    }

    at_send("ATE0", 1000);
    at_send("AT+CMEE=2", 1000);

    /* 查询和配置 GPS 天线类型（常见: 0=主动天线, 1=被动天线） */
    /* 有些模块默认天线没电，需要设置天线类型 */
    at_send("AT+QGPSCFG=\"gpsantennatype\",0", 2000);  /* 主动天线（默认），通常需要供电 */
    at_send("AT+QGPSCFG=\"gpsantennatype\",1", 2000);  /* 也试一下被动天线 */
    
    /* 查询 GPS firmware 版本 */
    at_send("AT+QGPSVER?", 2000);

    /* 先关闭可能存在的旧 GPS session */
    at_send("AT+QGPSEND", 2000);

    /* 开启 GPS（不指定 NMEA 端口，使用默认值） */
    /* AT+QGPS=1 开启独立 GPS */
    at_send("AT+QGPS=1", 3000);

    /* 检查 GPS 状态 */
    at_send("AT+QGPS?", 2000);

    ESP_LOGI(TAG, "gps-only init done, waiting 60s for first fix...");
    vTaskDelay(pdMS_TO_TICKS(60000));  /* 给 GPS 模块足够时间冷启动定位 */

    int fix_fail_count = 0;

    while (true) {
        ESP_LOGI(TAG, "-> AT+QGPSLOC=2");
        gsm4g_send_line("AT+QGPSLOC=2");

        bool got_loc = false;
        TickType_t start = xTaskGetTickCount();
        while ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS < 10000) {
            uint32_t remain = 10000 - (xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
            int n = read_line(line, sizeof(line), remain > 50 ? remain : 50);
            if (n <= 0) continue;
            ESP_LOGI(TAG, "<- %s", line);
            if (strstr(line, "+QGPSLOC:")) {
                char *p = strchr(line, ':');
                if (!p) continue;
                p++;
                char *tok = strtok(p, ",");
                int idx = 0;
                double lat = 0, lon = 0;
                while (tok) {
                    while (*tok == ' ') tok++;
                    if (idx == 0) {
                        /* UTC time */
                    } else if (idx == 1) {
                        lat = atof(tok);
                    } else if (idx == 2) {
                        lon = atof(tok);
                    }
                    tok = strtok(NULL, ",");
                    idx++;
                }
                if (lat != 0.0 && lon != 0.0) {
                    if (xSemaphoreTake(s_gps_lock, portMAX_DELAY) == pdTRUE) {
                        s_last_gps.valid = true;
                        s_last_gps.lat = lat;
                        s_last_gps.lon = lon;
                        xSemaphoreGive(s_gps_lock);
                    }
                    ESP_LOGI(TAG, "GPS: lat=%.6f lon=%.6f", lat, lon);
                    got_loc = true;
                    fix_fail_count = 0;
                } else {
                    ESP_LOGW(TAG, "GPS: received but lat=%.6f lon=%.6f (fix=%d)", lat, lon, idx > 5 ? 1 : 0);
                }
            }
            if (strstr(line, "OK")) break;
            if (strstr(line, "ERROR") || strstr(line, "+CME ERROR")) {
                /* 记录错误，但继续尝试 */
                ESP_LOGW(TAG, "GPSLOC error: %s", line);
                break;
            }
        }
        if (!got_loc) {
            fix_fail_count++;
            ESP_LOGW(TAG, "GPS: no valid location (attempt %d)", fix_fail_count);

            /* 如果持续 2 分钟没定位，尝试重启 GPS 引擎 */
            if (fix_fail_count >= 24) {  /* 24 * ~15s ≈ 360s = 6分钟 */
                ESP_LOGW(TAG, "GPS: restarting GNSS engine...");
                at_send("AT+QGPSEND", 2000);
                vTaskDelay(pdMS_TO_TICKS(3000));
                at_send("AT+QGPS=1", 3000);
                at_send("AT+QGPS?", 2000);
                fix_fail_count = 0;
                ESP_LOGI(TAG, "GPS: engine restarted, waiting 60s");
                vTaskDelay(pdMS_TO_TICKS(60000));
                continue;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));  /* 10秒查询一次，减少模块负载，给更多时间定位 */
    }
}

esp_err_t gsm4g_gps_only_init(int uart_num, gpio_num_t tx, gpio_num_t rx,
                              gpio_num_t pwr_en, gpio_num_t reset_n)
{
    s_uart_num = uart_num;
    s_gps_lock = xSemaphoreCreateMutex();
    if (!s_gps_lock) return ESP_ERR_NO_MEM;

    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(uart_num, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_num, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(uart_num, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    hw_power_on(pwr_en, reset_n);
    xTaskCreate(gsm4g_gps_only_task, "gps_only", 4096, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t gsm4g_http_post_json(const char *url, const char *json_body)
{
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20000)) != pdTRUE) return ESP_ERR_TIMEOUT;

    esp_err_t result = ESP_FAIL;
    char cmd[128];

    do {
        snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%u,80", (unsigned)strlen(url));
        ESP_LOGI(TAG, "-> %s", cmd);
        gsm4g_send_line(cmd);
        if (wait_token("CONNECT", NULL, 0, 5000) != ESP_OK) break;
        gsm4g_send_raw(url, strlen(url));
        if (wait_token("OK", NULL, 0, 5000) != ESP_OK) break;

        snprintf(cmd, sizeof(cmd), "AT+QHTTPPOST=%u,80,80", (unsigned)strlen(json_body));
        ESP_LOGI(TAG, "-> %s", cmd);
        gsm4g_send_line(cmd);
        if (wait_token("CONNECT", NULL, 0, 10000) != ESP_OK) break;
        gsm4g_send_raw(json_body, strlen(json_body));

        char line[LINE_BUF_SIZE];
        TickType_t start = xTaskGetTickCount();
        while ((xTaskGetTickCount() - start) * portTICK_PERIOD_MS < 30000) {
            int n = read_line(line, sizeof(line), 30000);
            if (n <= 0) break;
            ESP_LOGI(TAG, "<- %s", line);
            if (strstr(line, "+QHTTPPOST:")) {
                int err = -1, status = 0;
                if (sscanf(line, "+QHTTPPOST: %d,%d", &err, &status) >= 2) {
                    if (err == 0 && status >= 200 && status < 300) {
                        result = ESP_OK;
                    } else if (status == 301 || status == 302 || status == 307 || status == 308) {
                        /* Cloudflare "Always Use HTTPS" 会把 http 请求 301 到 https。
                         * AT+QHTTP 不支持 HTTPS 也不跟随重定向，HTTP 明文上传必然失败。
                         * 请在 Cloudflare 控制台关闭 Always Use HTTPS（或对 /api/fall 放行 HTTP）。
                         * 见 wifi_upload.h 顶部说明。 */
                        ESP_LOGE(TAG, "HTTP %d -> 4G模块不支持HTTPS/重定向。请关闭Cloudflare 'Always Use HTTPS'后重试", status);
                    } else {
                        ESP_LOGE(TAG, "QHTTPPOST err=%d status=%d", err, status);
                    }
                }
                break;
            }
            if (strstr(line, "ERROR")) break;
        }
    } while (0);

    xSemaphoreGive(s_lock);
    return result;
}
