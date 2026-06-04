# AirPlay 2 Receiver for ESP32
cmake_minimum_required(VERSION 3.16)

# Read firmware version from version.txt
file(READ "${CMAKE_SOURCE_DIR}/version.txt" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)

# Add components directory
set(EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/version.cmake)
if(IDF_VERSION_MAJOR LESS 5 OR (IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR LESS 5))
    message(WARNING
        "ESP-IDF ${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}.${IDF_VERSION_PATCH} detected. "
        "This project is developed and tested against ESP-IDF >= 5.5. "
        "Older versions may require workarounds (e.g. for hosted-WiFi Kconfig symbols on ESP32-P4). "
        "Please upgrade with: cd $ENV{IDF_PATH} && git fetch && git checkout v5.5.2 && ./install.sh")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# cspot (vendored in components/cspot/) requires C++20. Set globally so the
# add_subdirectory() chain in that component picks it up consistently.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(airplay2-receiver VERSION ${PROJECT_VER})

# Globally expose include dirs that PIO ESP-IDF refuses to propagate via the
# normal REQUIRES mechanism on xtensa builds. Affects:
# - components/spotify_connect (needs cspot + mbedtls + pthread + nanopb)
# - components/dlna_renderer (needs esp_http_server + esp_audio_codec + lwip)
idf_build_set_property(COMPILE_OPTIONS
    "-I$ENV{IDF_PATH}/components/esp_http_server/include"
    "-I$ENV{IDF_PATH}/components/esp_http_client/include"
    "-I$ENV{IDF_PATH}/components/http_parser"
    "-I$ENV{IDF_PATH}/components/esp_netif/include"
    "-I$ENV{IDF_PATH}/components/esp_event/include"
    "-I$ENV{IDF_PATH}/components/mbedtls/mbedtls/include"
    "-I$ENV{IDF_PATH}/components/mbedtls/port/include"
    "-I${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_audio_codec/include"
    "-I${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_audio_codec/include/decoder"
    "-I${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_audio_codec/include/decoder/impl"
    APPEND)

include(cmake/fix_usb_device_uac_platformio.cmake)

# Build a SPIFFS image from data/ and flash it alongside the firmware.
# Partition label "storage" matches partitions.csv. Used by:
#   - components/dac_tas57xx/ (HybridFlow DAC firmware)
#   - components/display/     (ST7789 background image)
# Flash firmware + SPIFFS together with: idf.py build flash
spiffs_create_partition_image(storage "${CMAKE_SOURCE_DIR}/data" FLASH_IN_PROJECT)
