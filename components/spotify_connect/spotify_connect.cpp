// Spotify Connect shim — adapts cspot (squeezelite-esp32 vendored) to the
// airplay-esp32 audio pipeline. ZeroConf flow only.
//
// Derived from squeezelite-esp32 components/spotify/Shim.cpp (MIT) but
// rewritten against this project's APIs: audio_output_write() for PCM,
// nvs_flash directly for credentials, and our own URL decoder.

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ApResolve.h>
#include <BellTask.h>
#include <CSpotContext.h>
#include <CentralAudioBuffer.h>
#include <Logger.h>
#include <LoginBlob.h>
#include <MDNSService.h>
#include <PlainConnection.h>
#include <Session.h>
#include <SpircHandler.h>
#include <TrackPlayer.h>
#include <Utils.h>
#include <WrappedSemaphore.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "spotify_connect.h"

// Forward declarations from the main app (avoids a REQUIRES cycle with the
// "main" component). Must match signatures in main/audio/audio_output.h,
// main/audio/audio_receiver.h, and main/playback_control.h.
extern "C" {
esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait);
void audio_output_stop(void);
void audio_output_start(void);
void audio_output_flush(void);
void audio_output_set_sample_rate(uint32_t rate);
void audio_receiver_pause(void);

typedef enum {
  PLAYBACK_SOURCE_NONE,
  PLAYBACK_SOURCE_AIRPLAY,
  PLAYBACK_SOURCE_BLUETOOTH,
  PLAYBACK_SOURCE_SPOTIFY,
} playback_source_t;
void playback_control_set_source(playback_source_t source);
}

// Spotify Commercial Hardware credentials — supplied via SPOTIFY_SECRET
// env var at build (compile flags), else fall back to placeholders that will
// not authenticate. ZeroConf still works for the visibility/handshake part.
#if !defined(CLIENT_ID)
#define CLIENT_ID "<your client id>"
#endif
#if !defined(CLIENT_SECRET)
#define CLIENT_SECRET "<your client secret>"
#endif

static const char *TAG = "spotify_connect";
static const char *NVS_NS = "spotify";
static const char *NVS_KEY_CREDS = "credentials";
static const char *NVS_KEY_KEEPALIVE = "keepalive";

// ---------------------------------------------------------------------------
// In-place URL decode, copied minimally from squeezelite-esp32's tools.c (BSD).
static void url_decode(char *str) {
  char *src = str, *dst = str;
  while (*src) {
    if (*src == '%' && src[1] && src[2]) {
      char hex[3] = {src[1], src[2], 0};
      *dst++ = (char)strtol(hex, NULL, 16);
      src += 3;
    } else if (*src == '+') {
      *dst++ = ' ';
      src++;
    } else {
      *dst++ = *src++;
    }
  }
  *dst = 0;
}

// ---------------------------------------------------------------------------
// PCM sink — cspot delivers 16-bit interleaved stereo at 44.1 kHz.

class cspotPlayer;
static cspotPlayer *g_player = nullptr;

class cspotPlayer : public bell::Task {
 public:
  cspotPlayer(const char *name, httpd_handle_t server, int port)
      : bell::Task("cspot", 32 * 1024, 0, 0),
        name_(name),
        server_(server),
        port_(port) {
    loadCreds();
  }

  esp_err_t handleGET(httpd_req_t *req);
  esp_err_t handlePOST(httpd_req_t *req);

  // REST control endpoints under /api/spotify/
  esp_err_t apiStatus(httpd_req_t *req);
  esp_err_t apiKeepalive(httpd_req_t *req);
  esp_err_t apiForget(httpd_req_t *req);

  bool isActive() const { return state_.load() == LINKED && !paused_.load(); }
  bool hasCredentials() const { return !credentials_.empty(); }
  bool getKeepalive() const { return keepalive_.load(); }
  esp_err_t setKeepalive(bool enabled);
  esp_err_t forgetCredentials();
  void disconnect() {
    if (spirc_) spirc_->disconnect();
    state_.store(DISCO);
  }

 private:
  void runTask() override;
  void registerZeroConf();
  void onEvent(std::unique_ptr<cspot::SpircHandler::Event> event);
  size_t pcmWrite(uint8_t *pcm, size_t bytes, std::string_view trackId);

  void loadCreds();
  void saveCreds(const std::string &json);

  enum State { INIT, LINKED, DISCO, ABORT };
  std::atomic<State> state_{INIT};
  std::atomic<bool> paused_{false};
  std::atomic<bool> have_output_{false};
  std::atomic<bool> keepalive_{false};

  std::string name_;
  httpd_handle_t server_;
  int port_;

  std::string credentials_;
  bell::WrappedSemaphore clientConnected_;

  std::shared_ptr<cspot::LoginBlob> blob_;
  std::unique_ptr<cspot::SpircHandler> spirc_;

  int volume_ = (UINT16_MAX * 60) / 100;  // ~60%

  // Track current sample rate (only 44.1 kHz observed in practice).
  uint32_t sample_rate_ = 44100;
};

// ---------------------------------------------------------------------------
// NVS helpers
void cspotPlayer::loadCreds() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  size_t len = 0;
  if (nvs_get_str(h, NVS_KEY_CREDS, NULL, &len) == ESP_OK && len > 1) {
    credentials_.resize(len);
    nvs_get_str(h, NVS_KEY_CREDS, credentials_.data(), &len);
    credentials_.resize(len - 1);  // strip trailing NUL
  }
  uint8_t ka = 0;
  if (nvs_get_u8(h, NVS_KEY_KEEPALIVE, &ka) == ESP_OK) {
    keepalive_.store(ka != 0);
  }
  nvs_close(h);
}

esp_err_t cspotPlayer::setKeepalive(bool enabled) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
  nvs_set_u8(h, NVS_KEY_KEEPALIVE, enabled ? 1 : 0);
  nvs_commit(h);
  nvs_close(h);
  keepalive_.store(enabled);
  ESP_LOGI(TAG, "keepalive -> %d (takes effect at next session boundary)",
           enabled);
  return ESP_OK;
}

esp_err_t cspotPlayer::forgetCredentials() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
  nvs_erase_key(h, NVS_KEY_CREDS);
  nvs_set_u8(h, NVS_KEY_KEEPALIVE, 0);  // can't auto-connect without creds
  nvs_commit(h);
  nvs_close(h);
  credentials_.clear();
  keepalive_.store(false);
  // Force current session (if any) to tear down so next iter is ZeroConf.
  disconnect();
  ESP_LOGI(TAG, "credentials wiped; falling back to ZeroConf");
  return ESP_OK;
}

void cspotPlayer::saveCreds(const std::string &json) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, NVS_KEY_CREDS, json.c_str());
  nvs_commit(h);
  nvs_close(h);
}

// ---------------------------------------------------------------------------
// HTTP /spotify_info + /api/spotify/* handlers
extern "C" {
static esp_err_t cspot_http_get(httpd_req_t *req) {
  return g_player ? g_player->handleGET(req) : ESP_FAIL;
}
static esp_err_t cspot_http_post(httpd_req_t *req) {
  return g_player ? g_player->handlePOST(req) : ESP_FAIL;
}
static esp_err_t cspot_api_status(httpd_req_t *r);
static esp_err_t cspot_api_keepalive(httpd_req_t *r);
static esp_err_t cspot_api_forget(httpd_req_t *r);
}

esp_err_t cspotPlayer::handleGET(httpd_req_t *req) {
  if (!blob_) return ESP_ERR_HTTPD_INVALID_REQ;
  std::string body = blob_->buildZeroconfInfo();
  if (body.empty()) return ESP_ERR_HTTPD_INVALID_REQ;
  httpd_resp_set_hdr(req, "Content-type", "application/json");
  httpd_resp_send(req, body.c_str(), body.size());
  return ESP_OK;
}

esp_err_t cspotPlayer::handlePOST(httpd_req_t *req) {
  cJSON *resp = cJSON_CreateObject();
  cJSON_AddNumberToObject(resp, "status", 101);
  cJSON_AddStringToObject(resp, "statusString", "OK");
  cJSON_AddNumberToObject(resp, "spotifyError", 0);

  if (req->content_len) {
    char *body = (char *)calloc(1, req->content_len + 1);
    httpd_req_recv(req, body, req->content_len);
    url_decode(body);

    std::map<std::string, std::string> q;
    char *key = strtok(body, "&");
    while (key) {
      char *val = strchr(key, '=');
      if (val) {
        *val++ = '\0';
        q[key] = val;
      }
      key = strtok(NULL, "&");
    }
    free(body);

    if (blob_) {
      blob_->loadZeroconfQuery(q);
      clientConnected_.give();
    }
  }

  char *s = cJSON_PrintUnformatted(resp);
  cJSON_Delete(resp);
  httpd_resp_set_hdr(req, "Content-type", "application/json");
  esp_err_t rc = httpd_resp_send(req, s, strlen(s));
  free(s);
  return rc;
}

// ---------------------------------------------------------------------------
// cspot wiring

void cspotPlayer::registerZeroConf() {
  static bool registered = false;
  if (registered) return;  // idempotent — only do this once per boot
  httpd_uri_t get = {"/spotify_info", HTTP_GET, cspot_http_get, NULL};
  httpd_uri_t post = {"/spotify_info", HTTP_POST, cspot_http_post, NULL};
  httpd_register_uri_handler(server_, &get);
  httpd_register_uri_handler(server_, &post);

  // REST control API
  httpd_uri_t api_status = {"/api/spotify/status", HTTP_GET,
                            cspot_api_status, NULL};
  httpd_uri_t api_ka = {"/api/spotify/keepalive", HTTP_POST,
                        cspot_api_keepalive, NULL};
  httpd_uri_t api_forget = {"/api/spotify/forget", HTTP_POST,
                            cspot_api_forget, NULL};
  httpd_register_uri_handler(server_, &api_status);
  httpd_register_uri_handler(server_, &api_ka);
  httpd_register_uri_handler(server_, &api_forget);

  ESP_LOGI(TAG, "ZeroConf + REST API registered on port %d", port_);

  bell::MDNSService::registerService(
      blob_->getDeviceName(), "_spotify-connect", "_tcp", "", port_,
      {{"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"}});
  registered = true;
}

// ---------------------------------------------------------------------------
// REST control API under /api/spotify/

esp_err_t cspotPlayer::apiStatus(httpd_req_t *req) {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddStringToObject(o, "deviceName",
                          blob_ ? blob_->getDeviceName().c_str() : name_.c_str());
  cJSON_AddBoolToObject(o, "keepalive", keepalive_.load());
  cJSON_AddBoolToObject(o, "haveCredentials", !credentials_.empty());
  cJSON_AddBoolToObject(o, "linked", state_.load() == LINKED);
  cJSON_AddBoolToObject(o, "active", isActive());
  char *s = cJSON_PrintUnformatted(o);
  cJSON_Delete(o);
  httpd_resp_set_type(req, "application/json");
  esp_err_t rc = httpd_resp_send(req, s, strlen(s));
  free(s);
  return rc;
}

esp_err_t cspotPlayer::apiKeepalive(httpd_req_t *req) {
  char body[64] = {0};
  int n = httpd_req_recv(req, body, sizeof(body) - 1);
  if (n <= 0) return ESP_FAIL;
  cJSON *in = cJSON_Parse(body);
  if (!in) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
    return ESP_FAIL;
  }
  cJSON *e = cJSON_GetObjectItem(in, "enabled");
  if (!cJSON_IsBool(e)) {
    cJSON_Delete(in);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "missing bool 'enabled'");
    return ESP_FAIL;
  }
  bool enabled = cJSON_IsTrue(e);
  cJSON_Delete(in);
  setKeepalive(enabled);
  return apiStatus(req);
}

esp_err_t cspotPlayer::apiForget(httpd_req_t *req) {
  forgetCredentials();
  return apiStatus(req);
}

extern "C" {
static esp_err_t cspot_api_status(httpd_req_t *r) {
  return g_player ? g_player->apiStatus(r) : ESP_FAIL;
}
static esp_err_t cspot_api_keepalive(httpd_req_t *r) {
  return g_player ? g_player->apiKeepalive(r) : ESP_FAIL;
}
static esp_err_t cspot_api_forget(httpd_req_t *r) {
  return g_player ? g_player->apiForget(r) : ESP_FAIL;
}
}

size_t cspotPlayer::pcmWrite(uint8_t *pcm, size_t bytes, std::string_view) {
  if (!have_output_.load()) {
    // First PCM of this session — take over from AirPlay.
    audio_receiver_pause();
    audio_output_stop();
    audio_output_flush();
    audio_output_set_sample_rate(sample_rate_);
    audio_output_start();
    playback_control_set_source(PLAYBACK_SOURCE_SPOTIFY);
    have_output_.store(true);
  }
  // Block up to 100 ms so cspot back-pressures naturally on DMA full.
  esp_err_t rc = audio_output_write(pcm, bytes, pdMS_TO_TICKS(100));
  return (rc == ESP_OK) ? bytes : 0;
}

void cspotPlayer::onEvent(std::unique_ptr<cspot::SpircHandler::Event> ev) {
  using E = cspot::SpircHandler::EventType;
  switch (ev->eventType) {
    case E::PLAYBACK_START:
      ESP_LOGI(TAG, "PLAYBACK_START");
      spirc_->setRemoteVolume(volume_);
      sample_rate_ = 44100;
      break;
    case E::PLAY_PAUSE: {
      bool p = std::get<bool>(ev->data);
      paused_.store(p);
      ESP_LOGI(TAG, "%s", p ? "PAUSE" : "PLAY");
      break;
    }
    case E::FLUSH:
    case E::NEXT:
    case E::PREV:
      ESP_LOGI(TAG, "FLUSH/SKIP");
      audio_output_flush();
      break;
    case E::DISC:
      ESP_LOGI(TAG, "DISC");
      state_.store(DISCO);
      break;
    case E::VOLUME:
      volume_ = std::get<int>(ev->data);
      break;
    case E::DEPLETED:
      ESP_LOGI(TAG, "playlist depleted");
      break;
    default:
      break;
  }
}

void cspotPlayer::runTask() {
  blob_ = std::make_shared<cspot::LoginBlob>(name_);
  ESP_LOGI(TAG, "cspot ready: name=%s id=%s keepalive=%d have_creds=%d",
           blob_->getDeviceName().c_str(), blob_->getDeviceId().c_str(),
           keepalive_.load(), !credentials_.empty());

  // Always advertise via ZeroConf so the device stays discoverable in the app
  // even when keepalive is on (lets the user switch accounts or re-pair).
  registerZeroConf();

  // First iteration: skip ZeroConf wait only if keepalive AND we have creds.
  bool useZeroConf = !(keepalive_.load() && !credentials_.empty());
  if (!useZeroConf) {
    blob_->loadJson(credentials_);
    ESP_LOGI(TAG, "Keepalive: reusing stored credentials");
  }

  while (true) {
    if (useZeroConf) {
      ESP_LOGI(TAG, "Waiting for ZeroConf connect...");
      clientConnected_.wait();
    }
    ESP_LOGI(TAG, "Connecting to Spotify AP...");

    auto ctx = cspot::Context::createFromBlob(blob_);
    ctx->config.audioFormat = AudioFormat_OGG_VORBIS_160;
    ctx->session->connectWithRandomAp();
    ctx->config.authData = ctx->session->authenticate(blob_);
    ctx->config.clientId = CLIENT_ID;
    ctx->config.clientSecret = CLIENT_SECRET;

    if (ctx->config.authData.empty()) {
      ESP_LOGE(TAG, "auth failed");
      ctx.reset();
      if (!useZeroConf) {
        registerZeroConf();
        useZeroConf = true;
      }
      continue;
    }

    // Persist credentials asynchronously (NVS write outside cspot task).
    // Also cache in-memory so keepalive on next iteration can pick them up
    // without needing a reboot.
    {
      std::string js = ctx->getCredentialsJson();
      credentials_ = js;
      TimerHandle_t t = xTimerCreate(
          "cspot-cred", 1, pdFALSE, strdup(js.c_str()),
          [](TimerHandle_t timer) {
            auto *p = (char *)pvTimerGetTimerID(timer);
            nvs_handle_t h;
            if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
              nvs_set_str(h, NVS_KEY_CREDS, p);
              nvs_commit(h);
              nvs_close(h);
            }
            free(p);
            xTimerDelete(timer, portMAX_DELAY);
          });
      xTimerStart(t, portMAX_DELAY);
    }

    spirc_ = std::make_unique<cspot::SpircHandler>(ctx);
    state_.store(LINKED);

    spirc_->getTrackPlayer()->setDataCallback(
        [this](uint8_t *d, size_t n, std::string_view id) {
          return pcmWrite(d, n, id);
        });
    spirc_->setEventHandler(
        [this](std::unique_ptr<cspot::SpircHandler::Event> e) {
          onEvent(std::move(e));
        });

    ctx->session->startTask();

    while (state_.load() == LINKED) {
      ctx->session->handlePacket();
    }

    spirc_->disconnect();
    spirc_.reset();
    ctx.reset();

    // Release audio output back to AirPlay.
    if (have_output_.exchange(false)) {
      audio_output_stop();
      audio_output_flush();
      playback_control_set_source(PLAYBACK_SOURCE_NONE);
      audio_output_start();
    }

    ESP_LOGI(TAG, "Spotify session ended");
    if (state_.load() == ABORT) break;
    state_.store(INIT);

    // Keepalive: reload creds + rebuild blob so the next iteration reconnects
    // without waiting for a fresh ZeroConf click. ZeroConf-only mode falls
    // back to waiting on the semaphore.
    if (keepalive_.load() && !credentials_.empty()) {
      blob_ = std::make_shared<cspot::LoginBlob>(name_);
      blob_->loadJson(credentials_);
      useZeroConf = false;
    } else {
      useZeroConf = true;
    }
  }
}

// ---------------------------------------------------------------------------
// C API
extern "C" esp_err_t spotify_connect_init(const char *device_name,
                                          httpd_handle_t http_server,
                                          int http_port) {
  if (g_player) return ESP_ERR_INVALID_STATE;
  if (!device_name || !http_server) return ESP_ERR_INVALID_ARG;

  bell::setDefaultLogger();
  bell::enableTimestampLogging(true);

  g_player = new cspotPlayer(device_name, http_server, http_port);
  g_player->startTask();
  return ESP_OK;
}

extern "C" void spotify_connect_disconnect(void) {
  if (g_player) g_player->disconnect();
}

extern "C" bool spotify_connect_is_active(void) {
  return g_player ? g_player->isActive() : false;
}

extern "C" esp_err_t spotify_connect_set_keepalive(bool enabled) {
  return g_player ? g_player->setKeepalive(enabled) : ESP_ERR_INVALID_STATE;
}

extern "C" bool spotify_connect_get_keepalive(void) {
  return g_player ? g_player->getKeepalive() : false;
}

extern "C" bool spotify_connect_has_credentials(void) {
  return g_player ? g_player->hasCredentials() : false;
}

extern "C" esp_err_t spotify_connect_forget_credentials(void) {
  return g_player ? g_player->forgetCredentials() : ESP_ERR_INVALID_STATE;
}
