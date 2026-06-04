# DLNA / UPnP MediaRenderer for ESP-IDF.
# Stub-only when CONFIG_DLNA_RENDERER_ENABLE is off so the rest of the project
# can REQUIRES us without dragging in mbedtls + http client + audio codec.

if(NOT CONFIG_DLNA_RENDERER_ENABLE)
    idf_component_register(INCLUDE_DIRS "include")
    return()
endif()

idf_component_register(
    SRCS
        "dlna_renderer.c"
        "dlna_ssdp.c"
        "dlna_http.c"
        "dlna_didl.c"
        "dlna_audio.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES
        esp_http_server
        esp_http_client
        esp_netif
        esp_wifi
        mbedtls
        lwip
        freertos
        espressif__esp_audio_codec
)

# PIO ESP-IDF on xtensa silently drops target_include_directories for this
# component. Workaround: attach via per-source COMPILE_FLAGS.
set(_dlna_incs
    "-I$ENV{IDF_PATH}/components/esp_http_server/include"
    "-I$ENV{IDF_PATH}/components/esp_http_client/include"
    "-I$ENV{IDF_PATH}/components/esp_netif/include"
    "-I$ENV{IDF_PATH}/components/mbedtls/mbedtls/include"
    "-I$ENV{IDF_PATH}/components/mbedtls/port/include"
    "-I$ENV{IDF_PATH}/components/http_parser"
    "-I$ENV{IDF_PATH}/components/esp_event/include"
    "-I$ENV{IDF_PATH}/components/esp_eth/include"
    "-I${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/espressif__esp_audio_codec/include"
    "-I${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/espressif__esp_audio_codec/include/decoder"
    "-I${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/espressif__esp_audio_codec/include/decoder/impl"
)
string(REPLACE ";" " " _dlna_incs_str "${_dlna_incs}")
foreach(_src dlna_renderer.c dlna_ssdp.c dlna_http.c dlna_didl.c dlna_audio.c)
    set_source_files_properties(${_src} PROPERTIES COMPILE_FLAGS
                                "${_dlna_incs_str}")
endforeach()
