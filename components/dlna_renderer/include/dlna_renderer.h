#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DLNA / UPnP Media Renderer for ESP-IDF.
 *
 * Accepts "Cast to device" / "Play to" requests from Android (system cast),
 * BubbleUPnP, Plex/Plexamp, foobar2000 mobile, VLC, Synology DS Audio, etc.
 *
 * Implements minimum-viable UPnP MediaRenderer:1 device template:
 *   - SSDP discovery on UDP multicast 239.255.255.250:1900
 *   - HTTP-served root device descriptor + AVTransport + ConnectionManager SCPDs
 *   - SOAP control endpoints: SetAVTransportURI, Play, Pause, Stop,
 *     GetTransportInfo, GetPositionInfo, GetMediaInfo, GetProtocolInfo
 *   - GENA SUBSCRIBE stub (200 OK, no notifies — clients poll)
 *
 * Audio path:
 *   - On SetAVTransportURI+Play, fetches URL via esp_http_client
 *   - Decodes via esp_audio_codec (MP3/FLAC/AAC/OGG/Opus/PCM/ALAC)
 *   - Pipes PCM into existing audio_output_write()
 *
 * Coexists with AirPlay + Spotify Connect via playback_control source switch.
 */

/**
 * Initialize the DLNA renderer.
 *
 * Must be called after Wi-Fi is up, mDNS is initialized, and the shared
 * esp_http_server is running.
 *
 * @param http_server   Existing httpd handle. We register /dlna URIs on it.
 * @param http_port     The port http_server listens on (used in LOCATION URLs).
 * @param friendly_name Name shown in "Cast to device" pickers (max ~64 chars).
 */
esp_err_t dlna_renderer_init(httpd_handle_t http_server, uint16_t http_port,
                             const char *friendly_name);

/**
 * Stop the renderer (sends SSDP byebye, stops the SSDP task, releases audio).
 */
void dlna_renderer_deinit(void);

/**
 * @return true if a control point is currently streaming.
 */
bool dlna_renderer_is_active(void);

#ifdef __cplusplus
}
#endif
