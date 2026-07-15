#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool valid;
    double lat;
    double lon;
} gsm4g_gps_t;

esp_err_t gsm4g_init(int uart_num, gpio_num_t tx, gpio_num_t rx,
                     gpio_num_t pwr_en, gpio_num_t reset_n);

esp_err_t gsm4g_gps_only_init(int uart_num, gpio_num_t tx, gpio_num_t rx,
                              gpio_num_t pwr_en, gpio_num_t reset_n);

bool gsm4g_is_ready(void);

bool gsm4g_get_gps(gsm4g_gps_t *out);

esp_err_t gsm4g_http_post_json(const char *url, const char *json_body);

void gsm4g_get_imei(char *out, size_t len);

#ifdef __cplusplus
}
#endif
