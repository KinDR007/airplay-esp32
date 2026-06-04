// Audio fetch + decode for the DLNA renderer.
//
// On Play, spawns a task that:
//   1. Opens an esp_http_client on the URI.
//   2. Detects format from Content-Type header (or filename extension).
//   3. Opens an esp_audio_codec decoder for that format.
//   4. Pumps chunks: HTTP read → decode → audio_output_write.
//   5. Updates DLNA transport state on first PCM (TRANSITIONING → PLAYING)
//      and on end-of-stream / error / external stop.
//
// On Pause: stops feeding the output (audio_output_stop).
// On Stop: signals task to exit, joins.

#include "dlna_internal.h"

#include <string.h>

#include "esp_audio_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_dec_reg.h"
#include "esp_audio_types.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern esp_err_t audio_output_write(const void *data, size_t bytes,
                                    TickType_t wait);
extern void audio_output_stop(void);
extern void audio_output_start(void);
extern void audio_output_flush(void);
extern void audio_output_set_sample_rate(uint32_t rate);

static const char *TAG = "dlna_audio";

// Single playback context (only one stream at a time).
typedef struct {
  esp_http_client_handle_t client;
  esp_audio_dec_handle_t   dec;
  esp_audio_type_t         dec_type;
  uint8_t                 *raw_buf;
  uint8_t                 *pcm_buf;
  size_t                   raw_buf_sz;
  size_t                   pcm_buf_sz;
  volatile bool            paused;
  uint32_t                 sample_rate;
  uint32_t                 start_ms;  // for position tracking
} dlna_play_t;

static dlna_play_t s_play;

// Match Content-Type / extension to esp_audio_type_t. Returns ESP_AUDIO_TYPE_UNSUPPORT
// on unknown.
static esp_audio_type_t guess_format(const char *content_type,
                                     const char *uri) {
  if (content_type) {
    if (strstr(content_type, "audio/mpeg") || strstr(content_type, "audio/mp3"))
      return ESP_AUDIO_TYPE_MP3;
    if (strstr(content_type, "audio/aac") || strstr(content_type, "audio/mp4"))
      return ESP_AUDIO_TYPE_AAC;
    if (strstr(content_type, "audio/flac") || strstr(content_type, "x-flac"))
      return ESP_AUDIO_TYPE_FLAC;
    if (strstr(content_type, "audio/ogg")) return ESP_AUDIO_TYPE_VORBIS;
    if (strstr(content_type, "audio/wav") || strstr(content_type, "x-wav"))
      return ESP_AUDIO_TYPE_PCM;
    if (strstr(content_type, "audio/opus")) return ESP_AUDIO_TYPE_OPUS;
  }
  // Fall back to URL extension.
  const char *dot = strrchr(uri, '.');
  if (dot) {
    if (!strcasecmp(dot, ".mp3")) return ESP_AUDIO_TYPE_MP3;
    if (!strcasecmp(dot, ".m4a") || !strcasecmp(dot, ".aac"))
      return ESP_AUDIO_TYPE_AAC;
    if (!strcasecmp(dot, ".flac")) return ESP_AUDIO_TYPE_FLAC;
    if (!strcasecmp(dot, ".ogg") || !strcasecmp(dot, ".oga"))
      return ESP_AUDIO_TYPE_VORBIS;
    if (!strcasecmp(dot, ".wav")) return ESP_AUDIO_TYPE_PCM;
    if (!strcasecmp(dot, ".opus")) return ESP_AUDIO_TYPE_OPUS;
  }
  return ESP_AUDIO_TYPE_UNSUPPORT;
}

// Register only the decoders we actually need (saves binary footprint).
static bool s_decoders_registered = false;
static void ensure_decoders_registered(void) {
  if (s_decoders_registered) return;
  esp_mp3_dec_register();
  esp_aac_dec_register();
  esp_flac_dec_register();
  esp_vorbis_dec_register();
  esp_pcm_dec_register();
  esp_opus_dec_register();
  s_decoders_registered = true;
}

static void cleanup(void) {
  if (s_play.dec) {
    esp_audio_dec_close(s_play.dec);
    s_play.dec = NULL;
  }
  if (s_play.client) {
    esp_http_client_close(s_play.client);
    esp_http_client_cleanup(s_play.client);
    s_play.client = NULL;
  }
  free(s_play.raw_buf);
  free(s_play.pcm_buf);
  s_play.raw_buf = s_play.pcm_buf = NULL;
}

static void play_task(void *arg) {
  (void)arg;
  ensure_decoders_registered();

  esp_http_client_config_t cfg = {
      .url = g_dlna.current_uri,
      .timeout_ms = 8000,
      .buffer_size = 4096,
      .keep_alive_enable = false,
  };
  s_play.client = esp_http_client_init(&cfg);
  if (!s_play.client) {
    ESP_LOGE(TAG, "http init failed");
    goto done;
  }
  esp_err_t rc = esp_http_client_open(s_play.client, 0);
  if (rc != ESP_OK) {
    ESP_LOGE(TAG, "http open: %s", esp_err_to_name(rc));
    goto done;
  }
  int64_t content_len = esp_http_client_fetch_headers(s_play.client);
  int status = esp_http_client_get_status_code(s_play.client);
  ESP_LOGI(TAG, "HTTP %d, length=%lld", status, content_len);
  if (status / 100 != 2) goto done;

  // Detect format.
  char *content_type = NULL;
  esp_http_client_get_header(s_play.client, "Content-Type", &content_type);
  s_play.dec_type = guess_format(content_type, g_dlna.current_uri);
  if (s_play.dec_type == ESP_AUDIO_TYPE_UNSUPPORT) {
    ESP_LOGE(TAG, "unsupported format (Content-Type=%s)",
             content_type ? content_type : "?");
    goto done;
  }

  esp_audio_dec_cfg_t dec_cfg = {
      .type = s_play.dec_type,
      .cfg = NULL,
      .cfg_sz = 0,
  };
  if (esp_audio_dec_open(&dec_cfg, &s_play.dec) != ESP_OK) {
    ESP_LOGE(TAG, "decoder open type=%d failed", s_play.dec_type);
    goto done;
  }

  s_play.raw_buf_sz = 4096;
  s_play.pcm_buf_sz = 16384;
  s_play.raw_buf = heap_caps_malloc(s_play.raw_buf_sz, MALLOC_CAP_SPIRAM);
  s_play.pcm_buf = heap_caps_malloc(s_play.pcm_buf_sz, MALLOC_CAP_SPIRAM);
  if (!s_play.raw_buf || !s_play.pcm_buf) goto done;

  audio_output_flush();
  audio_output_start();
  bool first_pcm = true;

  size_t pending = 0;
  while (!g_dlna.audio_stop_request) {
    if (s_play.paused) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    int n = esp_http_client_read(s_play.client,
                                 (char *)s_play.raw_buf + pending,
                                 s_play.raw_buf_sz - pending);
    if (n <= 0) {
      ESP_LOGI(TAG, "EOF / read=%d", n);
      break;
    }
    pending += n;

    esp_audio_dec_in_raw_t in = {.buffer = s_play.raw_buf, .len = pending,
                                 .consumed = 0};
    esp_audio_dec_out_frame_t out = {.buffer = s_play.pcm_buf,
                                     .len = s_play.pcm_buf_sz};
    rc = esp_audio_dec_process(s_play.dec, &in, &out);
    if (rc == ESP_AUDIO_ERR_OK || rc == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
      if (out.decoded_size > 0) {
        if (first_pcm) {
          esp_audio_dec_info_t info = {0};
          esp_audio_dec_get_info(s_play.dec, &info);
          s_play.sample_rate = info.sample_rate ? info.sample_rate : 44100;
          audio_output_set_sample_rate(s_play.sample_rate);
          xSemaphoreTake(g_dlna.mtx, portMAX_DELAY);
          g_dlna.state = DLNA_STATE_PLAYING;
          xSemaphoreGive(g_dlna.mtx);
          first_pcm = false;
          ESP_LOGI(TAG, "PLAYING %luHz / %d ch", (unsigned long)info.sample_rate,
                   info.channel);
        }
        audio_output_write(s_play.pcm_buf, out.decoded_size, pdMS_TO_TICKS(200));
      }
    }
    // Consume what decoder accepted; move leftover to start of raw_buf.
    if (in.consumed > 0 && in.consumed <= pending) {
      memmove(s_play.raw_buf, s_play.raw_buf + in.consumed,
              pending - in.consumed);
      pending -= in.consumed;
    } else if (in.consumed == 0 && pending == s_play.raw_buf_sz) {
      // Decoder didn't consume anything and we're full → garbage; drop.
      pending = 0;
    }
  }

done:
  cleanup();
  xSemaphoreTake(g_dlna.mtx, portMAX_DELAY);
  g_dlna.state = DLNA_STATE_STOPPED;
  g_dlna.audio_active = false;
  xSemaphoreGive(g_dlna.mtx);
  audio_output_stop();
  ESP_LOGI(TAG, "play task exit");
  g_dlna.audio_task = NULL;
  vTaskDelete(NULL);
}

esp_err_t dlna_audio_play_uri(const char *uri) {
  (void)uri;  // current_uri is the authoritative source
  if (g_dlna.audio_task) {
    g_dlna.audio_stop_request = true;
    for (int i = 0; i < 50 && g_dlna.audio_task; i++)
      vTaskDelay(pdMS_TO_TICKS(20));
  }
  g_dlna.audio_stop_request = false;
  s_play.paused = false;
  g_dlna.audio_active = true;
  if (xTaskCreatePinnedToCore(play_task, "dlna_play", 6144, NULL, 12,
                              &g_dlna.audio_task, 1) != pdPASS) {
    ESP_LOGE(TAG, "play task spawn failed");
    g_dlna.audio_active = false;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void dlna_audio_pause(bool pause) { s_play.paused = pause; }

void dlna_audio_stop(void) {
  if (!g_dlna.audio_task) return;
  g_dlna.audio_stop_request = true;
  for (int i = 0; i < 50 && g_dlna.audio_task; i++)
    vTaskDelay(pdMS_TO_TICKS(20));
}
