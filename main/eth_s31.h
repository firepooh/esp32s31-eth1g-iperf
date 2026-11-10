/*
 * ESP32-S31-Function-CoreBoard-1 Gigabit Ethernet bring-up helpers
 * (internal EMAC, RGMII, Motorcomm YT8531 PHY)
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_eth.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Install EMAC + PHY driver, attach netif and start Ethernet.
 *        IP: DHCP first, static fallback after CONFIG_ETH_TEST_DHCP_TIMEOUT_S.
 */
esp_err_t eth_s31_init(void);

/** @brief Print link / speed / IP summary */
void eth_s31_print_info(void);

/** @brief Dump and decode PHY registers (link quality, 1000BASE-T status, idle error count) */
void eth_s31_dump_phy(void);

/** @brief Switch to static IP (gw may be NULL) */
esp_err_t eth_s31_set_static_ip(const char *ip, const char *mask, const char *gw);

/** @brief Switch back to DHCP client */
esp_err_t eth_s31_use_dhcp(void);

/**
 * @brief Change auto-negotiation advertisement and restart the link.
 * @param gigabit true: advertise 1000/100/10, false: advertise 100/10 only
 */
esp_err_t eth_s31_set_adv(bool gigabit);

#ifdef __cplusplus
}
#endif
