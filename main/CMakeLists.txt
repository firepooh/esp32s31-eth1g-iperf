idf_component_register(SRCS "cmd_ethernet.c"
                            "eth_s31.c"
                            "ethernet_iperf_main.c"
                       PRIV_REQUIRES fatfs esp_netif esp_eth esp_timer console
                       INCLUDE_DIRS ".")
