idf_component_register(
    SRC_DIRS  "."
    INCLUDE_DIRS "."
    PRIV_REQUIRES nvs_flash mbedtls esp-tls esp_driver_gpio esp_driver_uart ieee802154 esp_wifi
                  esp_http_client
)
