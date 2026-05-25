# CMakeLists.txt for the main component of the ESP-IDF project
idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "." "../../common/include"
                       REQUIRES esp_netif
                       PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_netif wifi_provisioning lwip driver esp_driver_tsens led_strip)
