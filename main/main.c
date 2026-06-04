#include "audio_output.h"
#include "audio_receiver.h"
#include "audio_stream.h"
#include "buttons.h"
#include "spiram_task.h"
#include "display.h"
#include "dns_server.h"
#include "ethernet.h"
#include "led.h"
#include "hap.h"
#include "mdns_airplay.h"
#include "nvs_flash.h"
#include "playback_control.h"
#include "ptp_clock.h"
#include "rtsp_server.h"
#include "settings.h"
#include "web_server.h"
#include "log_stream.h"
#include "wifi.h"
#include "spiffs_storage.h"

#include "rtsp_events.h"
#ifdef CONFIG_NOWPLAYING_UI_ENABLE
#include "now_playing_store.h"
#endif

#ifdef CONFIG_BT_A2DP_ENABLE
#include "a2dp_sink.h"
#endif

#ifdef CONFIG_SPOTIFY_CONNECT_ENABLE
#include "spotify_connect.h"
#endif

#ifdef CONFIG_DLNA_RENDERER_ENABLE
#include "dlna_renderer.h"
#endif

#include "iot_board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

// AP mode IP address (192.168.4.1 in network byte order)
#define AP_IP_ADDR 0x0104A8C0

static bool s_airplay_started = false;
static bool s_airplay_infrastructure_ready = false;

static void start_airplay_services(void) {
  if (s_airplay_started) {
    return;
  }

  ESP_LOGI(TAG, "Starting AirPlay services...");

  // One-time infrastructure init (PTP, HAP, audio receiver/output)
  if (!s_airplay_infrastructure_ready) {
    esp_err_t err = ptp_clock_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "Failed to init PTP clock: %s", esp_err_to_name(err));
      s_airplay_started = false;
      return;
    }

    ESP_ERROR_CHECK(hap_init());
    ESP_ERROR_CHECK(audio_receiver_init());
    ESP_ERROR_CHECK(audio_output_init());
    mdns_airplay_init();
#ifdef CONFIG_DLNA_RENDERER_ENABLE
    {
      char dlna_name[65];
      settings_get_device_name(dlna_name, sizeof(dlna_name));
      httpd_handle_t srv = web_server_get_handle();
      uint16_t port = web_server_get_port();
      if (srv) {
        esp_err_t e = dlna_renderer_init(srv, port, dlna_name);
        if (e != ESP_OK)
          ESP_LOGW(TAG, "DLNA renderer init failed: %s", esp_err_to_name(e));
        else
          ESP_LOGI(TAG, "DLNA renderer ready");
      }
    }
#endif
#ifdef CONFIG_SPOTIFY_CONNECT_ENABLE
    {
      ESP_LOGI(TAG, "Initializing Spotify Connect...");
      char name[65];
      settings_get_device_name(name, sizeof(name));
      httpd_handle_t srv = web_server_get_handle();
      uint16_t port = web_server_get_port();
      ESP_LOGI(TAG, "  name=%s srv=%p port=%u", name, srv, port);
      if (srv) {
        esp_err_t e = spotify_connect_init(name, srv, port);
        if (e != ESP_OK) {
          ESP_LOGW(TAG, "Spotify Connect init failed: %s", esp_err_to_name(e));
        } else {
          ESP_LOGI(TAG, "Spotify Connect ready (port %u)", port);
        }
      } else {
        ESP_LOGW(TAG, "Web server not running, skipping Spotify Connect");
      }
    }
#endif
    s_airplay_infrastructure_ready = true;
  }

  audio_output_start();

  ESP_ERROR_CHECK(rtsp_server_start());

  s_airplay_started = true;
  playback_control_set_source(PLAYBACK_SOURCE_AIRPLAY);
  ESP_LOGI(TAG, "AirPlay ready");
}
#ifdef CONFIG_BT_A2DP_ENABLE
static void stop_airplay_services(void) {
  if (!s_airplay_started) {
    return;
  }

  ESP_LOGI(TAG, "Stopping AirPlay services...");

  rtsp_server_stop();
  audio_output_stop();

  s_airplay_started = false;
  playback_control_set_source(PLAYBACK_SOURCE_NONE);
  ESP_LOGI(TAG, "AirPlay stopped");
}
#endif

static void network_monitor_task(void *pvParameters) {
  (void)pvParameters;
  bool had_network = ethernet_is_connected() || wifi_is_connected();
  bool dns_running = !had_network;
  bool wifi_started = wifi_is_connected() || !ethernet_is_connected();
  bool had_eth = ethernet_is_connected();

  // Start captive portal DNS if no network yet
  if (dns_running) {
    dns_server_start(AP_IP_ADDR);
  }

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    bool eth_up = ethernet_is_connected();
    bool wifi_up = wifi_is_connected();
    bool has_network = eth_up || wifi_up;

    // Ethernet just came up — stop WiFi entirely
    if (eth_up && !had_eth && wifi_started) {
      ESP_LOGI(TAG, "Ethernet connected — stopping WiFi");
      wifi_stop();
      wifi_started = false;
      wifi_up = false;
    }

    // Ethernet dropped — bring up WiFi (AP + STA)
    if (!eth_up && had_eth) {
      ESP_LOGI(TAG, "Ethernet down — starting WiFi as fallback");
      wifi_init_apsta(NULL, NULL);
      wifi_started = true;
    }

    had_eth = eth_up;
    has_network = eth_up || wifi_is_connected();

    if (has_network == had_network) {
      continue;
    }

    if (has_network) {
      ESP_LOGI(TAG, "Network up (eth=%s, wifi=%s)", eth_up ? "yes" : "no",
               wifi_up ? "yes" : "no");
      start_airplay_services();
      if (dns_running) {
        dns_server_stop();
        dns_running = false;
      }
    } else {
      if (!dns_running) {
        dns_server_start(AP_IP_ADDR);
        dns_running = true;
      }
    }

    had_network = has_network;
  }
}

// ---------------------------------------------------------------------------
// "Now playing" — logs one tidy line per track change + play/pause/stop.
// AirPlay metadata arrives via SETPARAMETER (DMAP-tagged or bplist progress).
// rtsp_handlers.c already parses it; we just surface it to the serial log.

static rtsp_metadata_t s_np_last;
static bool s_np_have = false;

static void on_now_playing(rtsp_event_t event, const rtsp_event_data_t *data,
                           void *user_data) {
  (void)user_data;
  switch (event) {
  case RTSP_EVENT_METADATA: {
    const rtsp_metadata_t *m = &data->metadata;
    // Progress-only updates carry no text — skip to avoid log spam.
    if (!m->title[0] && !m->artist[0]) break;
    bool track_changed = !s_np_have ||
                         strcmp(m->title, s_np_last.title) != 0 ||
                         strcmp(m->artist, s_np_last.artist) != 0;
    char dur[8];
    rtsp_format_time_mmss(m->duration_secs, dur, sizeof(dur));
    if (track_changed) {
      ESP_LOGI("now_playing", "NOW PLAYING: %s - %s [%s] (%s)",
               m->artist[0] ? m->artist : "?",
               m->title[0] ? m->title : "?",
               m->album[0] ? m->album : "-", dur);
      if (m->genre[0])
        ESP_LOGI("now_playing", "  genre: %s", m->genre);
    }
    s_np_last = *m;
    s_np_have = true;
    break;
  }
  case RTSP_EVENT_PLAYING:
    ESP_LOGI("now_playing", "PLAY");
    break;
  case RTSP_EVENT_PAUSED:
    ESP_LOGI("now_playing", "PAUSE");
    break;
  case RTSP_EVENT_DISCONNECTED:
    s_np_have = false;
    ESP_LOGI("now_playing", "STOP");
    break;
  default:
    break;
  }
}

#ifdef CONFIG_BT_A2DP_ENABLE
static void on_bt_state_changed(bool connected) {
  if (connected) {
    ESP_LOGI(TAG, "BT connected — disabling AirPlay + Wi-Fi");
    stop_airplay_services();
    playback_control_set_source(PLAYBACK_SOURCE_BLUETOOTH);
    // Drop Wi-Fi so BT (classic BR/EDR) doesn't compete with the WLAN radio
    // on the shared 2.4 GHz antenna. Eth (if used) stays up. AirPlay/Spotify
    // are network services so they go quiet automatically — they come back
    // when Wi-Fi is re-armed below on BT disconnect.
    if (!ethernet_is_connected()) {
      wifi_stop();
    }
  } else {
    ESP_LOGI(TAG, "BT disconnected — re-arming Wi-Fi + AirPlay");
    playback_control_set_source(PLAYBACK_SOURCE_NONE);
    if (!ethernet_is_connected() && !wifi_is_connected()) {
      wifi_init_apsta(NULL, NULL);
      // network_monitor_task will call start_airplay_services once IP is up.
    } else if (ethernet_is_connected() || wifi_is_connected()) {
      start_airplay_services();
    }
  }
}

static void on_airplay_client_event(rtsp_event_t event,
                                    const rtsp_event_data_t *data,
                                    void *user_data) {
  (void)data;
  (void)user_data;
  if (bt_a2dp_sink_is_connected()) {
    return;
  }
  switch (event) {
  case RTSP_EVENT_CLIENT_CONNECTED:
    ESP_LOGI(TAG, "AirPlay client connected — disabling BT");
    bt_a2dp_sink_set_discoverable(false);
    break;
  case RTSP_EVENT_PAUSED:
    // V1 grace period active — keep BT hidden so the phone reconnects
    // to AirPlay rather than falling back to BT.
    ESP_LOGI(TAG, "AirPlay paused — keeping BT hidden");
    break;
  case RTSP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "AirPlay client disconnected — enabling BT");
    bt_a2dp_sink_set_discoverable(true);
    break;
  default:
    break;
  }
}
#endif

void app_main(void) {
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_ERROR_CHECK(settings_init());
  spiffs_storage_init();
  log_stream_init();
  ESP_ERROR_CHECK(playback_control_init());
  led_init();

  // Initialize board-specific hardware (includes I2C/SPI bus for display and
  // DAC)
  ESP_LOGI(TAG, "Board: %s", iot_board_get_info());
  esp_err_t err = iot_board_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Board init failed: %s", esp_err_to_name(err));
  }

  // Pass the board-owned bus to the display so it reuses it rather than
  // creating a duplicate bus on the same pins.
#if defined(CONFIG_DISPLAY_BUS_SPI)
  display_init(iot_board_get_handle(BOARD_SPI_DISP_ID));
#else
  display_init(iot_board_get_handle(BOARD_I2C_DISP_ID));
#endif

  // Pre-allocate audio task stacks while internal heap is still unfragmented.
  // WiFi/TCP/TLS allocations fragment the heap, making large contiguous
  // allocations unreliable later.
  ESP_ERROR_CHECK(audio_realtime_preallocate());

  // Try ethernet first
  bool eth_available = false;
  err = ethernet_init();
  if (err == ESP_OK) {
    // Wait for ethernet link + DHCP (up to 5s for link, then 10s more for DHCP)
    ESP_LOGI(TAG, "Waiting for ethernet...");
    for (int i = 0; i < 25 && !ethernet_is_link_up(); i++) {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (ethernet_is_link_up() && !ethernet_is_connected()) {
      ESP_LOGI(TAG, "Ethernet link up, waiting for DHCP...");
      for (int i = 0; i < 50 && !ethernet_is_connected(); i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
      }
    }
    eth_available = ethernet_is_connected();
    if (eth_available) {
      ESP_LOGI(TAG, "Ethernet connected");
    } else {
      ESP_LOGI(TAG, "Ethernet not connected (cable?), will use WiFi");
    }
  } else if (err != ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGW(TAG, "Ethernet init failed: %s", esp_err_to_name(err));
  }

  // Start WiFi only if ethernet is not available
  if (!eth_available) {
    wifi_init_apsta(NULL, NULL);

    // Wait for initial WiFi connection if credentials exist
    if (settings_has_wifi_credentials()) {
      if (!wifi_wait_connected(30000)) {
        ESP_LOGI(TAG, "Connect to 'ESP32-AirPlay-Setup' -> http://192.168.4.1");
      }
    } else {
      ESP_LOGI(TAG, "Connect to 'ESP32-AirPlay-Setup' -> http://192.168.4.1");
    }
  } else {
    ESP_LOGI(TAG, "Ethernet connected — skipping WiFi");
  }

  // Start services that work on any interface
  web_server_start(80);
  task_create_spiram(network_monitor_task, "net_mon", 4096, NULL, 5, NULL,
                     NULL);

  bool connected = eth_available || wifi_is_connected();
  if (connected) {
    start_airplay_services();
  }

  // Surface AirPlay metadata + transport state to the serial log.
  rtsp_events_register(on_now_playing, NULL);
#ifdef CONFIG_NOWPLAYING_UI_ENABLE
  // Cache metadata + artwork for the web UI / display.
  now_playing_store_init();
#endif

#ifdef CONFIG_BT_A2DP_ENABLE
  // Initialize Bluetooth A2DP Sink
  {
    char bt_name[65];
    settings_get_device_name(bt_name, sizeof(bt_name));
    esp_err_t bt_err = bt_a2dp_sink_init(bt_name, on_bt_state_changed);
    if (bt_err != ESP_OK) {
      ESP_LOGE(TAG, "BT A2DP init failed: %s", esp_err_to_name(bt_err));
    } else {
      rtsp_events_register(on_airplay_client_event, NULL);
    }
  }
#endif

  buttons_init();

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
