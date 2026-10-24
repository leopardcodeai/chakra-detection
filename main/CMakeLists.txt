idf_component_register(SRCS "main.c"
    PRIV_REQUIRES app_display app_chakra esp_iris esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris nvs_flash log
        esp_wifi esp_netif esp_event esp_http_client lwip mbedtls esp-mosaico-bsp)
