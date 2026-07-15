#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set ups and starts a simple DNS server that will respond to all queries
 * with the soft AP's IP address
 *
 */
void start_dns_server(void);


#ifdef __cplusplus
}
#endif
