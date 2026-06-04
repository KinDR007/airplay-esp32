// DLNA / UPnP MediaRenderer top-level: init, lifecycle, UDN derivation,
// network info capture.

#include "dlna_renderer.h"
#include "dlna_internal.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/sha1.h"
#include "sdkconfig.h"

static const char *TAG = "dlna";

dlna_ctx_t g_dlna;

// Derive a stable, UPnP-spec-compliant UUID from the device's STA MAC.
// Variant: name-based (RFC 4122 §4.3, UUIDv5-style).
static void derive_udn(char *out) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  uint8_t h[20];
  mbedtls_sha1(mac, 6, h);
  h[6] = (h[6] & 0x0F) | 0x50;   // version 5
  h[8] = (h[8] & 0x3F) | 0x80;   // RFC 4122 variant
  snprintf(out, DLNA_UDN_LEN + 1,
           "uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x%02x%02x%02x%02x",
           h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9],
           h[10], h[11], h[12], h[13], h[14], h[15]);
}

static esp_err_t capture_local_ip(char *out) {
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (!sta) {
    sta = esp_netif_get_handle_from_ifkey("ETH_DEF");
  }
  if (!sta) return ESP_FAIL;
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(sta, &ip) != ESP_OK) return ESP_FAIL;
  if (ip.ip.addr == 0) return ESP_FAIL;
  snprintf(out, 16, IPSTR, IP2STR(&ip.ip));
  return ESP_OK;
}

esp_err_t dlna_renderer_init(httpd_handle_t http_server, uint16_t http_port,
                             const char *friendly_name) {
  if (g_dlna.mtx) {
    ESP_LOGW(TAG, "already initialised");
    return ESP_ERR_INVALID_STATE;
  }
  if (!http_server || !friendly_name) {
    return ESP_ERR_INVALID_ARG;
  }

  memset(&g_dlna, 0, sizeof(g_dlna));
  g_dlna.mtx = xSemaphoreCreateMutex();
  if (!g_dlna.mtx) return ESP_ERR_NO_MEM;

  strlcpy(g_dlna.friendly_name, friendly_name, sizeof(g_dlna.friendly_name));
  derive_udn(g_dlna.udn);

  if (capture_local_ip(g_dlna.local_ip) != ESP_OK) {
    ESP_LOGE(TAG, "no IPv4 yet — call after wifi up");
    vSemaphoreDelete(g_dlna.mtx);
    g_dlna.mtx = NULL;
    return ESP_ERR_INVALID_STATE;
  }
  g_dlna.http_port = http_port;
  g_dlna.httpd = http_server;
  g_dlna.state = DLNA_STATE_STOPPED;

  ESP_LOGI(TAG, "init: %s @ %s:%u  UDN=%s", g_dlna.friendly_name,
           g_dlna.local_ip, g_dlna.http_port, g_dlna.udn);

  esp_err_t err = dlna_http_register();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "http register failed");
    return err;
  }
  err = dlna_ssdp_start();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ssdp start failed");
    return err;
  }

  ESP_LOGI(TAG, "DLNA MediaRenderer ready");
  return ESP_OK;
}

void dlna_renderer_deinit(void) {
  dlna_ssdp_stop();
  dlna_audio_stop();
  if (g_dlna.mtx) {
    vSemaphoreDelete(g_dlna.mtx);
    g_dlna.mtx = NULL;
  }
}

bool dlna_renderer_is_active(void) {
  return g_dlna.state == DLNA_STATE_PLAYING;
}
