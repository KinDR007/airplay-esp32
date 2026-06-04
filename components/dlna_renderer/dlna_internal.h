// Shared state between dlna_renderer.c subsystems.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DLNA_URL_MAX       512
#define DLNA_METADATA_MAX  4096
#define DLNA_NAME_MAX      96
#define DLNA_UUID_LEN      36   // 8-4-4-4-12 hex
#define DLNA_UDN_LEN       (DLNA_UUID_LEN + 5)  // "uuid:" prefix

typedef enum {
  DLNA_STATE_STOPPED = 0,
  DLNA_STATE_PLAYING,
  DLNA_STATE_PAUSED,
  DLNA_STATE_TRANSITIONING,
  DLNA_STATE_NO_MEDIA,
} dlna_state_t;

typedef struct {
  // Identity
  char friendly_name[DLNA_NAME_MAX];
  char udn[DLNA_UDN_LEN + 1];     // "uuid:XXXX..."
  char local_ip[16];               // dotted IPv4 of our STA iface
  uint16_t http_port;
  httpd_handle_t httpd;

  // Mutex around transport state
  SemaphoreHandle_t mtx;

  // Transport state
  dlna_state_t state;
  char current_uri[DLNA_URL_MAX];
  char current_metadata[DLNA_METADATA_MAX];
  char track_title[128];
  char track_artist[128];
  char track_album[128];
  uint32_t track_duration_secs;
  uint32_t track_position_secs;

  // SSDP runtime
  TaskHandle_t ssdp_task;
  int ssdp_sock;
  volatile bool ssdp_stop;

  // Audio task
  TaskHandle_t audio_task;
  volatile bool audio_stop_request;
  volatile bool audio_active;
} dlna_ctx_t;

extern dlna_ctx_t g_dlna;

// ssdp
esp_err_t dlna_ssdp_start(void);
void dlna_ssdp_stop(void);

// http handlers + soap
esp_err_t dlna_http_register(void);

// audio fetcher / decoder
esp_err_t dlna_audio_play_uri(const char *uri);
void dlna_audio_pause(bool pause);
void dlna_audio_stop(void);

// didl-lite parse (best-effort)
void dlna_didl_extract(const char *xml, char *title, size_t title_cap,
                       char *artist, size_t artist_cap, char *album,
                       size_t album_cap, uint32_t *duration_secs);
