/*
 * "ethernet" console command for ESP32-S31 Gigabit Ethernet test
 */
#include <string.h>
#include <stdio.h>
#include "esp_console.h"
#include "esp_err.h"
#include "cmd_ethernet.h"
#include "eth_s31.h"

static void print_usage(void)
{
    printf("Usage:\n"
           "  ethernet info                    link/speed/IP summary\n"
           "  ethernet phy                     PHY register dump (1G status, idle error count)\n"
           "  ethernet speed <auto|100>        auto: advertise 1000/100/10, 100: limit to 100M\n"
           "  ethernet ip <addr> <mask> [gw]   set static IP\n"
           "  ethernet dhcp                    use DHCP client\n"
           "  ethernet stats                   EMAC RX drop counters (cleared on read)\n"
           "  ethernet flowctrl <on|off>       pause-frame flow control, restarts the link\n");
}

static int eth_cmd(int argc, char **argv)
{
    if (argc < 2) {
        print_usage();
        return 1;
    }
    const char *sub = argv[1];
    esp_err_t err = ESP_OK;

    if (!strcmp(sub, "info")) {
        eth_s31_print_info();
    } else if (!strcmp(sub, "phy")) {
        eth_s31_dump_phy();
    } else if (!strcmp(sub, "dhcp")) {
        err = eth_s31_use_dhcp();
    } else if (!strcmp(sub, "ip")) {
        if (argc < 4) {
            print_usage();
            return 1;
        }
        err = eth_s31_set_static_ip(argv[2], argv[3], argc > 4 ? argv[4] : NULL);
    } else if (!strcmp(sub, "speed")) {
        if (argc < 3 || (strcmp(argv[2], "auto") && strcmp(argv[2], "1000") && strcmp(argv[2], "100"))) {
            print_usage();
            return 1;
        }
        err = eth_s31_set_adv(strcmp(argv[2], "100") != 0);
    } else if (!strcmp(sub, "stats")) {
        eth_s31_print_stats();
    } else if (!strcmp(sub, "flowctrl")) {
        if (argc < 3 || (strcmp(argv[2], "on") && strcmp(argv[2], "off"))) {
            print_usage();
            return 1;
        }
        err = eth_s31_set_flow_ctrl(!strcmp(argv[2], "on"));
    } else {
        print_usage();
        return 1;
    }
    if (err != ESP_OK) {
        printf("failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    return 0;
}

void register_ethernet_commands(void)
{
    const esp_console_cmd_t cmd = {
        .command = "ethernet",
        .help = "Ethernet control: info | phy | speed <auto|100> | ip <addr> <mask> [gw] | dhcp | stats | flowctrl <on|off>",
        .hint = NULL,
        .func = eth_cmd,
        .argtable = NULL,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
}
