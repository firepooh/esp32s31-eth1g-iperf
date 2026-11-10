/*
 * ESP32-S31-Function-CoreBoard-1 Gigabit Ethernet bring-up helpers
 *
 * Board wiring (ESP-IDF v6.1 ESP32-S31 EMAC defaults, same as examples/ethernet/basic):
 *   MDC=GPIO5, MDIO=GPIO6, PHY_RST=GPIO7
 *   TXC=GPIO13, TX_CTL=GPIO12, TXD0..3=GPIO8..11
 *   RXC=GPIO14, RX_CTL=GPIO15, RXD0..3=GPIO19,18,17,16
 * PHY: Motorcomm YT8531 (driven by the IEEE 802.3 generic PHY driver + chip specific setup)
 */
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "sdkconfig.h"
#include "eth_s31.h"

static const char *TAG = "eth_s31";

/* IEEE 802.3 standard PHY registers */
#define PHY_REG_BMCR        0x00
#define PHY_REG_BMSR        0x01
#define PHY_REG_PHYID1      0x02
#define PHY_REG_PHYID2      0x03
#define PHY_REG_ANAR        0x04
#define PHY_REG_ANLPAR      0x05
#define PHY_REG_ANER        0x06
#define PHY_REG_GBCR        0x09
#define PHY_REG_GBSR        0x0A
#define PHY_REG_ESR         0x0F

/* GBCR (0x09) bits */
#define GBCR_ADV_1000FD     (1U << 9)
#define GBCR_ADV_1000HD     (1U << 8)
/* GBSR (0x0A) bits */
#define GBSR_MS_FAULT       (1U << 15)
#define GBSR_MS_MASTER      (1U << 14)
#define GBSR_LOCAL_RX_OK    (1U << 13)
#define GBSR_REMOTE_RX_OK   (1U << 12)
#define GBSR_LP_1000FD      (1U << 11)
#define GBSR_LP_1000HD      (1U << 10)
#define GBSR_IDLE_ERR_MASK  0xFFU

/* Motorcomm YT8531 specific registers */
#define MOTORCOMM_PHYID1    0x4F51
#define YT_REG_SSR          0x11    /* PHY specific status */
#define YT_REG_EXT_ADDR     0x1E
#define YT_REG_EXT_DATA     0x1F
#define YT_EXT_CHIP_CFG     0xA001
#define YT_EXT_RGMII_CFG1   0xA003
#define YT_EXT_LED0_CFG     0xA00C
#define YT_EXT_LED1_CFG     0xA00D
#define YT_EXT_LED2_CFG     0xA00E
#define YT_EXT_LED_BLINK    0xA00F

/* YT8531 SSR (0x11) fields */
#define YT_SSR_SPEED(v)     (((v) >> 14) & 0x3)
#define YT_SSR_DUPLEX_FULL  (1U << 13)
#define YT_SSR_RESOLVED     (1U << 11)
#define YT_SSR_LINK         (1U << 10)
#define YT_SSR_MDIX         (1U << 6)
#define YT_SSR_DOWNGRADE    (1U << 5)

static esp_eth_handle_t s_eth;
static esp_netif_t *s_netif;
static esp_timer_handle_t s_dhcp_timer;
static volatile bool s_link_up;
static volatile bool s_got_ip;
static volatile bool s_static_mode;
static bool s_is_motorcomm;
static bool s_adv_fallback_done;
static uint32_t s_phy_addr;

/* ---------------------------------------------------------------- PHY access */

static esp_err_t phy_rd(uint32_t reg, uint32_t *val)
{
    esp_eth_phy_reg_rw_data_t d = { .reg_addr = reg, .reg_value_p = val };
    return esp_eth_ioctl(s_eth, ETH_CMD_READ_PHY_REG, &d);
}

static esp_err_t phy_wr(uint32_t reg, uint32_t val)
{
    esp_eth_phy_reg_rw_data_t d = { .reg_addr = reg, .reg_value_p = &val };
    return esp_eth_ioctl(s_eth, ETH_CMD_WRITE_PHY_REG, &d);
}

static esp_err_t yt_ext_rd(uint32_t ext_reg, uint32_t *val)
{
    ESP_RETURN_ON_ERROR(phy_wr(YT_REG_EXT_ADDR, ext_reg), TAG, "ext addr write failed");
    return phy_rd(YT_REG_EXT_DATA, val);
}

static esp_err_t yt_ext_wr(uint32_t ext_reg, uint32_t val)
{
    ESP_RETURN_ON_ERROR(phy_wr(YT_REG_EXT_ADDR, ext_reg), TAG, "ext addr write failed");
    return phy_wr(YT_REG_EXT_DATA, val);
}

static int speed_to_mbps(eth_speed_t s)
{
    switch (s) {
    case ETH_SPEED_10M:   return 10;
    case ETH_SPEED_100M:  return 100;
    case ETH_SPEED_1000M: return 1000;
    default:              return -1;
    }
}

static int yt_ssr_mbps(uint32_t ssr)
{
    static const int tbl[4] = {10, 100, 1000, -1};
    return tbl[YT_SSR_SPEED(ssr)];
}

/* -------------------------------------------------------- PHY specific init */

static esp_err_t phy_specific_init(void)
{
    uint32_t id1 = 0, id2 = 0;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, ETH_CMD_G_PHY_ADDR, &s_phy_addr), TAG, "get PHY addr failed");
    ESP_RETURN_ON_ERROR(phy_rd(PHY_REG_PHYID1, &id1), TAG, "read PHYID1 failed");
    ESP_RETURN_ON_ERROR(phy_rd(PHY_REG_PHYID2, &id2), TAG, "read PHYID2 failed");
    s_is_motorcomm = (id1 == MOTORCOMM_PHYID1);
    ESP_LOGI(TAG, "PHY found: MDIO addr %" PRIu32 ", ID 0x%04" PRIX32 "_%04" PRIX32 " (%s)",
             s_phy_addr, id1, id2, s_is_motorcomm ? "Motorcomm YT85xx" : "unknown vendor");

    /* YT8531 disables auto-negotiation after the HW reset done by the generic driver -> re-enable */
    bool an = true;
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, ETH_CMD_S_AUTONEGO, &an), TAG, "enable autoneg failed");

#if CONFIG_ETH_TEST_YT8531_INIT
    if (!s_is_motorcomm) {
        ESP_LOGW(TAG, "PHY is not Motorcomm, skip YT8531 RGMII delay setup");
        return ESP_OK;
    }
    uint32_t v = 0;
    /* RX clock delay ~2 ns (Chip_Config 0xA001 bit8 rxc_dly_en) */
    ESP_RETURN_ON_ERROR(yt_ext_rd(YT_EXT_CHIP_CFG, &v), TAG, "read Chip_Config failed");
    v |= (1U << 8);
    ESP_RETURN_ON_ERROR(yt_ext_wr(YT_EXT_CHIP_CFG, v), TAG, "write Chip_Config failed");
    /* TX clock delay ~1.95 ns (RGMII_Config1 0xA003 tx_delay_sel[3:0], tx_delay_sel_fe[7:4] = 13 x 150 ps) */
    ESP_RETURN_ON_ERROR(yt_ext_rd(YT_EXT_RGMII_CFG1, &v), TAG, "read RGMII_Config1 failed");
    v = (v & ~0x00FFU) | (13U << 4) | 13U;
    ESP_RETURN_ON_ERROR(yt_ext_wr(YT_EXT_RGMII_CFG1, v), TAG, "write RGMII_Config1 failed");
    ESP_LOGI(TAG, "YT8531 RGMII delays set: RX ~2 ns, TX ~1.95 ns");
#endif
    return ESP_OK;
}

/* ------------------------------------------------------------- link checking */

static void adv_fallback_task(void *arg)
{
    ESP_LOGW(TAG, "Restarting link with 100M advertisement only (use 'ethernet speed auto' to retry 1G)");
    eth_s31_set_adv(false);
    vTaskDelete(NULL);
}

static void check_link_on_up(int mac_mbps)
{
    uint32_t gbsr = 0, anlpar = 0;
    phy_rd(PHY_REG_GBSR, &gbsr);
    phy_rd(PHY_REG_ANLPAR, &anlpar);
    bool lp_1000 = (gbsr & (GBSR_LP_1000FD | GBSR_LP_1000HD)) != 0;

    if (s_is_motorcomm) {
        uint32_t ssr = 0;
        phy_rd(YT_REG_SSR, &ssr);
        int phy_mbps = yt_ssr_mbps(ssr);
        ESP_LOGI(TAG, "PHY status: %d Mbps %s duplex, resolved=%d, %s, downgrade=%d (SSR=0x%04" PRIX32 ")",
                 phy_mbps, (ssr & YT_SSR_DUPLEX_FULL) ? "full" : "half", (ssr & YT_SSR_RESOLVED) ? 1 : 0,
                 (ssr & YT_SSR_MDIX) ? "MDIX" : "MDI", (ssr & YT_SSR_DOWNGRADE) ? 1 : 0, ssr);
        if (phy_mbps > 0 && phy_mbps != mac_mbps) {
            ESP_LOGE(TAG, "MAC/PHY speed mismatch: MAC %d Mbps vs PHY %d Mbps -> no traffic possible", mac_mbps, phy_mbps);
            if (lp_1000 && phy_mbps < 1000) {
                ESP_LOGE(TAG, "Partner can do 1000BASE-T but PHY downgraded: cable pair on pins 4-5 or 7-8 likely broken");
            }
            if (!s_adv_fallback_done) {
                s_adv_fallback_done = true;
                xTaskCreate(adv_fallback_task, "eth_adv_fb", 3072, NULL, 5, NULL);
            }
            return;
        }
    }

    if (mac_mbps == 1000) {
        ESP_LOGI(TAG, "1000BASE-T OK: role=%s, local_rx=%s, remote_rx=%s%s",
                 (gbsr & GBSR_MS_MASTER) ? "master" : "slave",
                 (gbsr & GBSR_LOCAL_RX_OK) ? "OK" : "NOK",
                 (gbsr & GBSR_REMOTE_RX_OK) ? "OK" : "NOK",
                 (gbsr & GBSR_MS_FAULT) ? ", MASTER/SLAVE FAULT" : "");
    } else {
        ESP_LOGW(TAG, "NOT gigabit (%d Mbps)", mac_mbps);
        if (!lp_1000) {
            ESP_LOGW(TAG, "Link partner does not advertise 1000BASE-T (100M-only switch/NIC, or 1G disabled)");
        } else {
            ESP_LOGW(TAG, "Partner advertises 1000BASE-T -> check cable (needs all 4 pairs, Cat5e or better)");
        }
    }
}

/* ------------------------------------------------------------ event handlers */

static void eth_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED: {
        eth_speed_t spd = ETH_SPEED_10M;
        eth_duplex_t dup = ETH_DUPLEX_HALF;
        esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &spd);
        esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &dup);
        int mbps = speed_to_mbps(spd);
        s_link_up = true;
        ESP_LOGI(TAG, "==== LINK UP: %d Mbps, %s duplex ====", mbps, dup == ETH_DUPLEX_FULL ? "full" : "half");
        check_link_on_up(mbps);
        if (!s_static_mode && !s_got_ip && CONFIG_ETH_TEST_DHCP_TIMEOUT_S > 0) {
            esp_timer_stop(s_dhcp_timer);
            esp_timer_start_once(s_dhcp_timer, (uint64_t)CONFIG_ETH_TEST_DHCP_TIMEOUT_S * 1000000ULL);
        }
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up = false;
        esp_timer_stop(s_dhcp_timer);
        ESP_LOGW(TAG, "==== LINK DOWN ====");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started, waiting for link (auto-negotiation)...");
        break;
    case ETHERNET_EVENT_STOP:
        s_link_up = false;
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        s_got_ip = true;
        esp_timer_stop(s_dhcp_timer);
        ESP_LOGI(TAG, "==== GOT IP (%s): " IPSTR "  mask " IPSTR "  gw " IPSTR " ====",
                 s_static_mode ? "static" : "DHCP",
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.netmask), IP2STR(&e->ip_info.gw));
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        s_got_ip = false;
        ESP_LOGW(TAG, "Lost IP address");
    }
}

static void dhcp_timeout_cb(void *arg)
{
    if (s_got_ip || s_static_mode) {
        return;
    }
    ESP_LOGW(TAG, "No DHCP reply within %d s -> fallback to static IP %s",
             CONFIG_ETH_TEST_DHCP_TIMEOUT_S, CONFIG_ETH_TEST_STATIC_IP);
    eth_s31_set_static_ip(CONFIG_ETH_TEST_STATIC_IP, CONFIG_ETH_TEST_STATIC_NETMASK, CONFIG_ETH_TEST_STATIC_GW);
}

/* -------------------------------------------------------------------- public */

esp_err_t eth_s31_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop create failed");

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_ETH_TEST_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_ETH_TEST_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();   /* RGMII, board default pins */
    ESP_LOGI(TAG, "EMAC RGMII: MDC=%d MDIO=%d RST=%d | TXC=%d TXCTL=%d TXD=%d,%d,%d,%d | RXC=%d RXCTL=%d RXD=%d,%d,%d,%d",
             emac.smi_gpio.mdc_num, emac.smi_gpio.mdio_num, CONFIG_ETH_TEST_PHY_RST_GPIO,
             emac.clock_config.rgmii.clock_tx_gpio, emac.emac_dataif_gpio.rgmii.tx_ctl_num,
             emac.emac_dataif_gpio.rgmii.txd0_num, emac.emac_dataif_gpio.rgmii.txd1_num,
             emac.emac_dataif_gpio.rgmii.txd2_num, emac.emac_dataif_gpio.rgmii.txd3_num,
             emac.clock_config.rgmii.clock_rx_gpio, emac.emac_dataif_gpio.rgmii.rx_ctl_num,
             emac.emac_dataif_gpio.rgmii.rxd0_num, emac.emac_dataif_gpio.rgmii.rxd1_num,
             emac.emac_dataif_gpio.rgmii.rxd2_num, emac.emac_dataif_gpio.rgmii.rxd3_num);

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_config);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "create EMAC instance failed");
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (phy == NULL) {
        mac->del(mac);
        ESP_LOGE(TAG, "create PHY instance failed");
        return ESP_FAIL;
    }

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&cfg, &s_eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "driver install failed (%s): PHY not answering on MDIO? check RST/MDC/MDIO", esp_err_to_name(err));
        mac->del(mac);
        phy->del(phy);
        s_eth = NULL;
        return err;
    }
    ESP_RETURN_ON_ERROR(phy_specific_init(), TAG, "PHY specific init failed");

    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&ncfg);
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "netif create failed");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth)), TAG, "netif attach failed");

    const esp_timer_create_args_t targs = { .callback = dhcp_timeout_cb, .name = "dhcp_fb" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&targs, &s_dhcp_timer), TAG, "timer create failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL), TAG, "reg eth evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, ip_event_handler, NULL), TAG, "reg ip evt");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, ip_event_handler, NULL), TAG, "reg ip evt");

    return esp_eth_start(s_eth);
}

void eth_s31_print_info(void)
{
    if (s_eth == NULL) {
        printf("Ethernet driver not installed\n");
        return;
    }
    uint8_t mac[6] = {0};
    eth_speed_t spd = ETH_SPEED_10M;
    eth_duplex_t dup = ETH_DUPLEX_HALF;
    bool an = false;
    esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac);
    esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &spd);
    esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &dup);
    esp_eth_ioctl(s_eth, ETH_CMD_G_AUTONEGO, &an);

    printf("HW addr   : " MACSTR "\n", MAC2STR(mac));
    printf("PHY       : addr %" PRIu32 ", %s\n", s_phy_addr, s_is_motorcomm ? "Motorcomm YT85xx" : "unknown");
    printf("Link      : %s\n", s_link_up ? "UP" : "DOWN");
    if (s_link_up) {
        printf("MAC speed : %d Mbps, %s duplex, autoneg %s\n", speed_to_mbps(spd),
               dup == ETH_DUPLEX_FULL ? "full" : "half", an ? "on" : "off");
    }
    if (s_is_motorcomm) {
        uint32_t ssr = 0;
        phy_rd(YT_REG_SSR, &ssr);
        printf("PHY status: link %s, %d Mbps, %s duplex, %s%s\n", (ssr & YT_SSR_LINK) ? "up" : "down",
               yt_ssr_mbps(ssr), (ssr & YT_SSR_DUPLEX_FULL) ? "full" : "half",
               (ssr & YT_SSR_MDIX) ? "MDIX" : "MDI", (ssr & YT_SSR_DOWNGRADE) ? ", DOWNGRADED" : "");
    }
    esp_netif_ip_info_t ip = {0};
    if (s_netif) {
        esp_netif_get_ip_info(s_netif, &ip);
    }
    printf("IP mode   : %s\n", s_static_mode ? "static" : "DHCP");
    printf("IP        : " IPSTR "  mask " IPSTR "  gw " IPSTR "\n",
           IP2STR(&ip.ip), IP2STR(&ip.netmask), IP2STR(&ip.gw));
}

void eth_s31_dump_phy(void)
{
    if (s_eth == NULL) {
        printf("Ethernet driver not installed\n");
        return;
    }
    static const struct {
        uint8_t reg;
        const char *name;
    } regs[] = {
        {PHY_REG_BMCR, "BMCR"}, {PHY_REG_BMSR, "BMSR"}, {PHY_REG_PHYID1, "PHYID1"}, {PHY_REG_PHYID2, "PHYID2"},
        {PHY_REG_ANAR, "ANAR"}, {PHY_REG_ANLPAR, "ANLPAR"}, {PHY_REG_ANER, "ANER"}, {PHY_REG_GBCR, "GBCR"},
        {PHY_REG_GBSR, "GBSR"}, {PHY_REG_ESR, "ESR"}, {YT_REG_SSR, "SSR(YT)"},
    };
    uint32_t val[sizeof(regs) / sizeof(regs[0])] = {0};

    printf("PHY @ MDIO addr %" PRIu32 "\n", s_phy_addr);
    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        if (phy_rd(regs[i].reg, &val[i]) != ESP_OK) {
            printf("  0x%02X %-8s : read failed\n", regs[i].reg, regs[i].name);
            continue;
        }
        printf("  0x%02X %-8s : 0x%04" PRIX32 "\n", regs[i].reg, regs[i].name, val[i]);
    }
    uint32_t bmsr = val[1], gbcr = val[7], gbsr = val[8], anlpar = val[5];

    printf("Decoded:\n");
    printf("  BMSR  : link %s, autoneg %s, remote fault %s\n", (bmsr & (1U << 2)) ? "up" : "down(latched)",
           (bmsr & (1U << 5)) ? "complete" : "not complete", (bmsr & (1U << 4)) ? "YES" : "no");
    printf("  GBCR  : advertise 1000FD=%d 1000HD=%d\n", (gbcr & GBCR_ADV_1000FD) ? 1 : 0, (gbcr & GBCR_ADV_1000HD) ? 1 : 0);
    printf("  ANLPAR: partner 100FD=%d 100HD=%d 10FD=%d 10HD=%d pause=%d\n",
           (int)((anlpar >> 8) & 1), (int)((anlpar >> 7) & 1), (int)((anlpar >> 6) & 1),
           (int)((anlpar >> 5) & 1), (int)((anlpar >> 10) & 1));
    printf("  GBSR  : partner 1000FD=%d 1000HD=%d, role=%s, local_rx=%s, remote_rx=%s, MS fault=%d\n",
           (gbsr & GBSR_LP_1000FD) ? 1 : 0, (gbsr & GBSR_LP_1000HD) ? 1 : 0,
           (gbsr & GBSR_MS_MASTER) ? "master" : "slave",
           (gbsr & GBSR_LOCAL_RX_OK) ? "OK" : "NOK", (gbsr & GBSR_REMOTE_RX_OK) ? "OK" : "NOK",
           (gbsr & GBSR_MS_FAULT) ? 1 : 0);
    printf("  GBSR  : idle error count = %" PRIu32 " (since last read, saturates at 255; >0 = 1G line errors)\n",
           gbsr & GBSR_IDLE_ERR_MASK);
    if (s_is_motorcomm) {
        uint32_t ssr = val[10];
        printf("  SSR   : link %s, %d Mbps, %s duplex, resolved=%d, %s, downgrade=%d\n",
               (ssr & YT_SSR_LINK) ? "up" : "down", yt_ssr_mbps(ssr), (ssr & YT_SSR_DUPLEX_FULL) ? "full" : "half",
               (ssr & YT_SSR_RESOLVED) ? 1 : 0, (ssr & YT_SSR_MDIX) ? "MDIX" : "MDI", (ssr & YT_SSR_DOWNGRADE) ? 1 : 0);
        uint32_t l0 = 0, l1 = 0, l2 = 0, lb = 0;
        yt_ext_rd(YT_EXT_LED0_CFG, &l0);
        yt_ext_rd(YT_EXT_LED1_CFG, &l1);
        yt_ext_rd(YT_EXT_LED2_CFG, &l2);
        yt_ext_rd(YT_EXT_LED_BLINK, &lb);
        printf("  LED cfg (ext 0xA00C..F): LED0=0x%04" PRIX32 " LED1=0x%04" PRIX32 " LED2=0x%04" PRIX32 " BLINK=0x%04" PRIX32 "\n",
               l0, l1, l2, lb);
    }
}

esp_err_t eth_s31_set_static_ip(const char *ip, const char *mask, const char *gw)
{
    ESP_RETURN_ON_FALSE(s_netif, ESP_ERR_INVALID_STATE, TAG, "netif not ready");
    esp_netif_ip_info_t info = {0};
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(ip, &info.ip), TAG, "invalid IP '%s'", ip);
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(mask, &info.netmask), TAG, "invalid netmask '%s'", mask);
    if (gw && gw[0]) {
        ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(gw, &info.gw), TAG, "invalid gateway '%s'", gw);
    }
    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED, err, TAG,
                        "DHCP stop failed: %s", esp_err_to_name(err));
    s_static_mode = true;
    esp_timer_stop(s_dhcp_timer);
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_netif, &info), TAG, "set IP failed");
    ESP_LOGI(TAG, "Static IP applied: " IPSTR " / " IPSTR " gw " IPSTR,
             IP2STR(&info.ip), IP2STR(&info.netmask), IP2STR(&info.gw));
    return ESP_OK;
}

esp_err_t eth_s31_use_dhcp(void)
{
    ESP_RETURN_ON_FALSE(s_netif, ESP_ERR_INVALID_STATE, TAG, "netif not ready");
    s_static_mode = false;
    s_got_ip = false;
    esp_err_t err = esp_netif_dhcpc_start(s_netif);
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED, err, TAG,
                        "DHCP start failed: %s", esp_err_to_name(err));
    ESP_LOGI(TAG, "DHCP client started");
    if (s_link_up && CONFIG_ETH_TEST_DHCP_TIMEOUT_S > 0) {
        esp_timer_stop(s_dhcp_timer);
        esp_timer_start_once(s_dhcp_timer, (uint64_t)CONFIG_ETH_TEST_DHCP_TIMEOUT_S * 1000000ULL);
    }
    return ESP_OK;
}

esp_err_t eth_s31_set_adv(bool gigabit)
{
    ESP_RETURN_ON_FALSE(s_eth, ESP_ERR_INVALID_STATE, TAG, "driver not installed");
    ESP_RETURN_ON_ERROR(esp_eth_stop(s_eth), TAG, "eth stop failed");
    uint32_t gbcr = 0;
    ESP_RETURN_ON_ERROR(phy_rd(PHY_REG_GBCR, &gbcr), TAG, "read GBCR failed");
    if (gigabit) {
        gbcr |= GBCR_ADV_1000FD;
        s_adv_fallback_done = false;
    } else {
        gbcr &= ~(GBCR_ADV_1000FD | GBCR_ADV_1000HD);
    }
    ESP_RETURN_ON_ERROR(phy_wr(PHY_REG_GBCR, gbcr), TAG, "write GBCR failed");
    ESP_LOGI(TAG, "Advertising %s, restarting auto-negotiation", gigabit ? "1000/100/10" : "100/10 only");
    return esp_eth_start(s_eth);
}
