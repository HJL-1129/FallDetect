#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void start_ota_task(char *url);
void update_ota_flag(bool bval);

#ifdef __cplusplus
}
#endif
