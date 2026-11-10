/*
 * ESP32-S31-Function-CoreBoard-1 : 1G Ethernet iperf test firmware (ESP-IDF v6.1)
 * Based on ESP-IDF examples/ethernet/iperf (Unlicense OR CC0-1.0)
 *
 * Console: USB Serial/JTAG (Type-C "USB Serial/JTAG" port)
 */
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_console.h"
#include "esp_vfs_fat.h"
#include "cmd_system.h"
#include "cmd_ethernet.h"
#include "iperf_cmd.h"
#include "eth_s31.h"

static const char *TAG = "main";

#if CONFIG_EXAMPLE_STORE_HISTORY
#define MOUNT_PATH "/data"
#define HISTORY_PATH MOUNT_PATH "/history.txt"

static void initialize_filesystem(void)
{
    static wl_handle_t wl_handle;
    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = 4,
        .format_if_mount_failed = true
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_PATH, "storage", &mount_config, &wl_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount FATFS (%s)", esp_err_to_name(err));
    }
}
#endif

void app_main(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
#if CONFIG_EXAMPLE_STORE_HISTORY
    initialize_filesystem();
    repl_config.history_save_path = HISTORY_PATH;
#endif
    repl_config.prompt = "iperf>";
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_config, &repl));

    /* Ethernet init failure is not fatal: keep the console alive for diagnosis */
    esp_err_t err = eth_s31_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet init failed: %s", esp_err_to_name(err));
    }

    register_system_common();
    iperf_cmd_register_iperf();
    register_ethernet_commands();

    printf("\n ==================== ESP32-S31 1G Ethernet iperf test ====================\n");
    printf("  ethernet info | ethernet phy      : link, speed, 1000BASE-T status\n");
    printf("  TCP RX : iperf -s -i 1                    | PC: iperf -c <ESP_IP> -t 10 -i 1\n");
    printf("  TCP TX : iperf -c <PC_IP> -t 10 -i 1      | PC: iperf -s -i 1\n");
    printf("  UDP RX : iperf -u -s -i 1                 | PC: iperf -u -c <ESP_IP> -b 1000M -t 10 -i 1\n");
    printf("  UDP TX : iperf -u -c <PC_IP> -t 10 -i 1   | PC: iperf -u -s -i 1\n");
    printf("  PC side must be iperf 2.x (not iperf3)\n");
    printf(" ===========================================================================\n\n");

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
