# Spotify Connect adapter — bridges cspot (vendored under components/cspot/)
# into the airplay-esp32 audio output and HTTP server.

set(CMAKE_CXX_STANDARD 20)

# When CONFIG_SPOTIFY_CONNECT_ENABLE is off (classic ESP32 builds), register an
# empty interface component so main.c can still REQUIRE us without pulling in
# cspot (which needs C++ exceptions, broken on Xtensa 14.2).
if(NOT CONFIG_SPOTIFY_CONNECT_ENABLE)
    idf_component_register(INCLUDE_DIRS "include")
    return()
endif()

idf_component_register(
    SRCS "spotify_connect.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        cspot
        esp_http_server
        json
        nvs_flash
        mdns
        freertos
        pthread
        mbedtls
        lwip
        driver
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-variable
    -Wno-format
)

# Workaround: PIO/IDF platform silently drops target_compile_definitions and
# add_definitions for this component on xtensa. Inject via CMAKE_CXX_FLAGS.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DBELL_ONLY_CJSON -DBELL_DISABLE_FMT -DPB_ENABLE_MALLOC -DPB_FIELD_32BIT" PARENT_SCOPE)

# Hardwire cspot + ESP-IDF include paths in case INTERFACE propagation via
# REQUIRES doesn't carry them (observed on xtensa-esp-elf + S3 toolchain).
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../cspot/cspot_lib/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../cspot/cspot_lib/bell/main/utilities/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../cspot/cspot_lib/bell/main/io/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../cspot/cspot_lib/bell/main/platform"
    "${CMAKE_BINARY_DIR}/esp-idf/cspot/cspot_lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/../cspot/cspot_lib/bell/external/nanopb"
    "$ENV{IDF_PATH}/components/pthread/include"
    "$ENV{IDF_PATH}/components/mbedtls/mbedtls/include"
    "$ENV{IDF_PATH}/components/mbedtls/port/include"
)

# Wait for cspot's generated protobuf headers before compiling our .cpp.
# generate_proto_sources is a custom target defined in cspot_lib/CMakeLists.txt
# (visible globally after add_subdirectory in components/cspot/).
add_dependencies(${COMPONENT_LIB} generate_proto_sources)
