deps_config := \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/app_trace/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/aws_iot/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/bt/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/driver/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/esp32/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/esp_adc_cal/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/esp_event/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/esp_http_client/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/esp_http_server/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp32-sniffer/components/espmqtt/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/ethernet/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/fatfs/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/freemodbus/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/freertos/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/heap/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/libsodium/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/log/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/lwip/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/mbedtls/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/mdns/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/mqtt/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/nvs_flash/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/openssl/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/pthread/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/spi_flash/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp32-sniffer/components/spiffs/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/tcpip_adapter/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/vfs/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/wear_levelling/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/wpa_supplicant/Kconfig \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/bootloader/Kconfig.projbuild \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/esptool_py/Kconfig.projbuild \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp32-sniffer/main/Kconfig.projbuild \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/components/partition_table/Kconfig.projbuild \
	/media/sda2/GitHubProjects/Personal/ESP32-WiFi-Sniffer/esp-idf/Kconfig

include/config/auto.conf: \
	$(deps_config)

ifneq "$(IDF_CMAKE)" "n"
include/config/auto.conf: FORCE
endif

$(deps_config): ;
