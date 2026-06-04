#include "now_playing_store.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "now_playing_store";

static SemaphoreHandle_t s_mtx;
static rtsp_metadata_t s_meta;
static bool s_have_text;
static uint8_t *s_artwork;     // in PSRAM
static size_t s_artwork_len;
static char s_artwork_mime[NOWPLAYING_MIME_MAX];
static const char *s_state = "stopped";
static uint32_t s_revision;

static void on_rtsp_event(rtsp_event_t event, const rtsp_event_data_t *data,
                          void *user_data) {
  (void)user_data;
  if (!s_mtx) return;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  switch (event) {
  case RTSP_EVENT_METADATA: {
    // iPhone alternates between full NowPlayingInfo (title/artist/...) and
    // progress-only updates (just elapsed/duration). Preserve cached text
    // when the new event is missing it.
    const rtsp_metadata_t *m = &data->metadata;

    // Detect track change BEFORE overwriting cached title/artist. On a new
    // track, reset duration + position even if iPhone's first NowPlayingInfo
    // doesn't include ElapsedTime — otherwise the web UI would keep
    // extrapolating off the previous track's position.
    bool track_changed = false;
    if (m->title[0] && strcmp(m->title, s_meta.title) != 0) {
      track_changed = true;
    } else if (m->artist[0] && strcmp(m->artist, s_meta.artist) != 0) {
      track_changed = true;
    }
    if (track_changed) {
      s_meta.position_secs = 0;
      s_meta.position_valid = false;
      s_meta.duration_secs = 0;
      s_meta.has_artwork = false;
    }

    if (m->title[0])
      strlcpy(s_meta.title, m->title, sizeof(s_meta.title));
    if (m->artist[0])
      strlcpy(s_meta.artist, m->artist, sizeof(s_meta.artist));
    if (m->album[0])
      strlcpy(s_meta.album, m->album, sizeof(s_meta.album));
    if (m->genre[0])
      strlcpy(s_meta.genre, m->genre, sizeof(s_meta.genre));
    if (m->duration_secs > 0)
      s_meta.duration_secs = m->duration_secs;
    if (m->position_valid) {
      s_meta.position_secs = m->position_secs;
      s_meta.position_valid = true;
    }
    if (m->has_artwork) s_meta.has_artwork = true;
    s_have_text = true;
    s_revision++;
    break;
  }
  case RTSP_EVENT_PLAYING:
    s_state = "playing";
    s_revision++;
    break;
  case RTSP_EVENT_PAUSED:
    s_state = "paused";
    s_revision++;
    break;
  case RTSP_EVENT_DISCONNECTED:
    s_state = "stopped";
    s_have_text = false;
    memset(&s_meta, 0, sizeof(s_meta));  // also clears position_valid
    s_artwork_len = 0;
    s_artwork_mime[0] = '\0';
    s_revision++;
    break;
  default:
    break;
  }
  xSemaphoreGive(s_mtx);
}

esp_err_t now_playing_store_init(void) {
  if (s_mtx) return ESP_OK;
  s_mtx = xSemaphoreCreateMutex();
  if (!s_mtx) return ESP_ERR_NO_MEM;

  s_artwork = heap_caps_malloc(NOWPLAYING_ARTWORK_MAX, MALLOC_CAP_SPIRAM);
  if (!s_artwork) {
    ESP_LOGW(TAG, "PSRAM artwork buffer (%dB) alloc failed — artwork disabled",
             NOWPLAYING_ARTWORK_MAX);
  }
  if (rtsp_events_register(on_rtsp_event, NULL) != 0) {
    ESP_LOGE(TAG, "rtsp_events_register failed");
    return ESP_FAIL;
  }
  ESP_LOGI(TAG, "now-playing store ready (%dB artwork buf)",
           s_artwork ? NOWPLAYING_ARTWORK_MAX : 0);
  return ESP_OK;
}

bool now_playing_store_get_text(rtsp_metadata_t *out) {
  if (!s_mtx || !out) return false;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  bool have = s_have_text;
  if (have) *out = s_meta;
  xSemaphoreGive(s_mtx);
  return have;
}

const char *now_playing_store_get_state(void) {
  // Pointer read of a static literal — atomic enough for status reporting.
  return s_state;
}

void now_playing_store_set_artwork(const uint8_t *data, size_t len,
                                   const char *mime) {
  if (!s_mtx || !s_artwork) return;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (len == 0 || !data) {
    s_artwork_len = 0;
    s_artwork_mime[0] = '\0';
  } else {
    size_t copy = len > NOWPLAYING_ARTWORK_MAX ? NOWPLAYING_ARTWORK_MAX : len;
    memcpy(s_artwork, data, copy);
    s_artwork_len = copy;
    if (mime && *mime) {
      strlcpy(s_artwork_mime, mime, sizeof(s_artwork_mime));
    } else {
      strlcpy(s_artwork_mime, "image/jpeg", sizeof(s_artwork_mime));
    }
  }
  s_revision++;
  xSemaphoreGive(s_mtx);
  if (len > 0) {
    ESP_LOGI(TAG, "artwork cached: %zuB %s", len > NOWPLAYING_ARTWORK_MAX
                                                 ? (size_t)NOWPLAYING_ARTWORK_MAX
                                                 : len,
             mime && *mime ? mime : "image/jpeg");
  }
}

size_t now_playing_store_get_artwork(uint8_t *out_buf, size_t out_cap,
                                     char *mime_out, size_t mime_cap) {
  if (!s_mtx || !s_artwork) return 0;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  size_t n = s_artwork_len;
  if (out_buf && out_cap > 0) {
    size_t copy = n > out_cap ? out_cap : n;
    memcpy(out_buf, s_artwork, copy);
    n = copy;  // bytes actually copied into caller's buffer
  }
  // out_buf == NULL or out_cap == 0 → probe mode: leave n = real length.
  if (mime_out && mime_cap > 0) {
    strlcpy(mime_out, s_artwork_mime[0] ? s_artwork_mime : "image/jpeg",
            mime_cap);
  }
  xSemaphoreGive(s_mtx);
  return n;
}

uint32_t now_playing_store_get_revision(void) {
  return s_revision;
}
