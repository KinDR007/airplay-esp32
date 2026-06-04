# cspot vendor component for ESP32-P4 airplay project.
# Wraps the upstream cspot + bell library (in cspot_lib/) as an ESP-IDF
# component so it can be linked into the main app.

set(CMAKE_CXX_STANDARD 20)

idf_component_register(
    SRCS ""
    INCLUDE_DIRS
        "cspot_lib/include"
        "cspot_lib/bell/main/utilities/include"
        "cspot_lib/bell/main/io/include"
        "cspot_lib/bell/main/platform"
    REQUIRES
        mbedtls
        mdns
        pthread
        driver
        lwip
        json
        esp_http_server
    LDFRAGMENTS "linker.lf"
)

# Skip the whole vendored cspot+bell build (no add_subdirectory) when Spotify
# Connect is disabled — saves build time and avoids the C++ exception linker
# requirement on Xtensa 14.2 toolchain. Include dirs above stay registered so
# anything that #includes cspot headers still finds them, but no library
# target is produced and nobody links it.
if(NOT CONFIG_SPOTIFY_CONNECT_ENABLE)
    return()
endif()

# Generated protobuf headers live in build/.../cspot_lib/protobuf/.
# Expose this dir to dependent components and to ourselves.
target_include_directories(${COMPONENT_LIB} INTERFACE
    "${CMAKE_CURRENT_BINARY_DIR}/cspot_lib"
)

# bell options — keep only what cspot needs.
# Match squeezelite-esp32's spotify/CMakeLists.txt.
set(BELL_DISABLE_CODECS ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_SINKS ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_FMT ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_REGEX ON CACHE BOOL "" FORCE)
set(BELL_ONLY_CJSON ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_WEBSERVER ON CACHE BOOL "" FORCE)
set(CSPOT_TARGET_ESP32 ON CACHE BOOL "" FORCE)

# Provide cJSON from ESP-IDF instead of bell's vendored copy.
set(BELL_EXTERNAL_CJSON "idf::json" CACHE STRING "" FORCE)

# Leave Vorbis empty — bell will build its own tremor decoder (in
# cspot_lib/bell/external/tremor/). The squeezelite project overrides this
# with its own codec component, but we don't have one.
set(BELL_EXTERNAL_VORBIS "" CACHE STRING "" FORCE)

# Spotify client credentials. Provide via SPOTIFY_SECRET env var at build:
#   export SPOTIFY_SECRET="-DCLIENT_ID=\\\"...\\\" -DCLIENT_SECRET=\\\"...\\\""
set(_secret $ENV{SPOTIFY_SECRET})
if(_secret)
    separate_arguments(_secret)
endif()

# Vendored CMakeLists in cspot_lib/ and bell/external/* were authored against
# CMake 2.x policies that modern CMake no longer accepts. Make the lower bound
# permissive for the whole vendored tree.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/cspot_lib ${CMAKE_CURRENT_BINARY_DIR}/cspot_lib)

# Silence non-fatal warnings in third-party code.
target_compile_options(cspot PRIVATE
    -Wno-unused-variable
    -Wno-unused-const-variable
    -Wno-maybe-uninitialized
    -Wno-misleading-indentation
    -Wno-stringop-overflow
    -Wno-stringop-overread
    -Wno-format
)
target_compile_options(bell PRIVATE
    -Wno-unused-variable
    -Wno-unused-const-variable
    -Wno-maybe-uninitialized
    -Wno-misleading-indentation
    -Wno-stringop-overflow
    -Wno-stringop-overread
    -Wno-format
)

# Expose cspot (and transitively bell) to dependents of this component.
target_link_libraries(${COMPONENT_LIB} INTERFACE cspot)

# Ensure the vendored cspot/bell libs (and their generated protobuf headers)
# are built before anything that REQUIRES this component starts compiling.
add_dependencies(${COMPONENT_LIB} cspot bell generate_proto_sources)
