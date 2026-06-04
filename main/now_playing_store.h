#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "rtsp_events.h"

/**
 * Thread-safe cache of the latest AirPlay "now playing" state.
 *
 * Text (title/artist/album/genre/duration/position) is captured automatically
 * by subscribing to RTSP_EVENT_METADATA. Artwork bytes (JPEG/PNG) are pushed
 * separately by the /command handler when iPhone includes them in the bplist.
 *
 * Web server and display consume the cache via the getters below.
 */

#define NOWPLAYING_ARTWORK_MAX (256 * 1024)  // headroom for hi-res covers
#define NOWPLAYING_MIME_MAX    32

esp_err_t now_playing_store_init(void);

/** Copy the latest text metadata. Returns false if nothing has been seen yet. */
bool now_playing_store_get_text(rtsp_metadata_t *out);

/** "playing" | "paused" | "stopped" — based on the last RTSP_EVENT_* seen. */
const char *now_playing_store_get_state(void);

/**
 * Push artwork bytes. Copies into the internal PSRAM buffer (truncates if
 * larger than NOWPLAYING_ARTWORK_MAX). Pass len=0 to clear.
 */
void now_playing_store_set_artwork(const uint8_t *data, size_t len,
                                   const char *mime);

/**
 * Copy current artwork into out_buf. Returns bytes written (0 if none).
 * mime_out is populated with the MIME type (e.g. "image/jpeg") if non-NULL.
 */
size_t now_playing_store_get_artwork(uint8_t *out_buf, size_t out_cap,
                                     char *mime_out, size_t mime_cap);

/** Monotonic counter — bumps on every text or artwork update. Use for ETag. */
uint32_t now_playing_store_get_revision(void);
