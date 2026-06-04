#pragma once

#include "esp_err.h"

/**
 * Web server for control panel
 * Provides:
 * - WiFi configuration
 * - Device name configuration
 * - OTA update
 */

/**
 * Initialize and start the web server
 * @param port HTTP server port (default: 80)
 */
esp_err_t web_server_start(uint16_t port);

/**
 * Stop the web server
 */
void web_server_stop(void);

/**
 * Get the underlying esp_http_server handle for components that want to
 * register additional URIs (e.g. Spotify Connect /spotify_info).
 * Returns NULL if web_server_start has not run.
 */
#include "esp_http_server.h"
httpd_handle_t web_server_get_handle(void);
uint16_t web_server_get_port(void);
