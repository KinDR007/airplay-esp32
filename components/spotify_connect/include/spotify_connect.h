#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Spotify Connect receiver built on cspot (squeezelite-esp32 vendored).
 *
 * Coexists with AirPlay: when a Spotify client connects, the active source
 * is switched away from AirPlay and decoded PCM is fed into the shared I2S
 * output via audio_output_write(). When the Spotify session ends, AirPlay
 * is allowed to resume.
 *
 * ZeroConf only — no Spotify OAuth credential storage. The receiver appears
 * in the Spotify app's "Connect to a device" list via _spotify-connect._tcp
 * mDNS service and the /spotify_info HTTP endpoint.
 */

/**
 * Initialize Spotify Connect.
 *
 * Must be called AFTER Wi-Fi is up, the HTTP server is started, and mdns_init()
 * has run. Spawns a background task that runs the cspot session.
 *
 * @param device_name  Service name advertised over mDNS (e.g. "Living Room").
 * @param http_server  Existing httpd handle. cspot registers /spotify_info on it.
 * @param http_port    Port the http_server listens on (advertised in mDNS TXT).
 */
esp_err_t spotify_connect_init(const char *device_name,
                               httpd_handle_t http_server, int http_port);

/**
 * Force-disconnect the current Spotify session (if any).
 * Safe to call from any task.
 */
void spotify_connect_disconnect(void);

/**
 * Returns true if a Spotify client is currently streaming.
 */
bool spotify_connect_is_active(void);

/**
 * Keep-alive (non-ZeroConf) mode.
 *
 * When enabled AND stored credentials exist, the device authenticates with
 * Spotify at startup and keeps the Mercury session alive even when no client
 * is actively playing. The device then shows up everywhere Spotify lists it
 * (other Spotify apps, WebAPI, Home Assistant) without anyone needing to
 * click "Connect" first.
 *
 * Default: disabled (pure ZeroConf — only visible to clients on the same LAN
 * until the first manual Connect, then they re-discover via mDNS).
 *
 * Setting takes effect at the next session boundary (immediately if idle).
 * Stored in NVS namespace "spotify", key "keepalive".
 */
esp_err_t spotify_connect_set_keepalive(bool enabled);
bool spotify_connect_get_keepalive(void);

/**
 * True if a Spotify credentials blob is stored in NVS (i.e. ZeroConf has
 * succeeded at least once and we can auto-connect on next boot).
 */
bool spotify_connect_has_credentials(void);

/**
 * Wipe stored credentials and disable keepalive. Forces fresh ZeroConf
 * pairing on the next session.
 */
esp_err_t spotify_connect_forget_credentials(void);

#ifdef __cplusplus
}
#endif
