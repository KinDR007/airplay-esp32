// HTTP endpoints for the UPnP MediaRenderer:
//   GET  /dlna/desc.xml                              — root device descriptor
//   GET  /dlna/AVTransport.xml                       — AVT SCPD
//   GET  /dlna/ConnectionManager.xml                 — CMR SCPD
//   POST /dlna/AVTransport/ctrl                      — SOAP control
//   POST /dlna/ConnectionManager/ctrl                — SOAP control
//   SUBSCRIBE /dlna/{AVTransport,ConnectionManager}/evt — GENA stub (200 OK)

#include "dlna_internal.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"

#include "descriptors/avtransport_scpd.h"
#include "descriptors/connmgr_scpd.h"
#include "descriptors/root_desc_template.h"

static const char *TAG = "dlna_http";

// ---------------------------------------------------------------------------
// Helpers

static esp_err_t send_xml(httpd_req_t *req, const char *body) {
  httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_sendstr(req, body);
}

// Substitute %s placeholders 1× and 2× into a fixed-size buffer. We use a
// 4 KB buffer because the root descriptor is ~1.2 KB after substitution.
static esp_err_t serve_root_desc(httpd_req_t *req) {
  char *buf = malloc(4096);
  if (!buf) return ESP_ERR_NO_MEM;
  snprintf(buf, 4096, DLNA_ROOT_DESC_TEMPLATE, g_dlna.friendly_name,
           g_dlna.udn);
  esp_err_t rc = send_xml(req, buf);
  free(buf);
  return rc;
}

static esp_err_t serve_avt_scpd(httpd_req_t *req) {
  return send_xml(req, DLNA_AVT_SCPD);
}
static esp_err_t serve_cmr_scpd(httpd_req_t *req) {
  return send_xml(req, DLNA_CMR_SCPD);
}

// ---------------------------------------------------------------------------
// SOAP parsing: forgiving string scan rather than full XML.

// Find <tag>…</tag> and copy contents into out_buf. Stops on first match.
// Returns 0 on success, -1 if not found.
static int xml_get_text(const char *xml, const char *tag, char *out,
                        size_t out_cap) {
  char open[64];
  snprintf(open, sizeof(open), "<%s>", tag);
  const char *p = strstr(xml, open);
  if (!p) {
    // Try with namespace prefix: <ns:tag>
    char with_colon[64];
    snprintf(with_colon, sizeof(with_colon), ":%s>", tag);
    p = strstr(xml, with_colon);
    if (!p) return -1;
    p += strlen(with_colon);
  } else {
    p += strlen(open);
  }
  char close[64];
  snprintf(close, sizeof(close), "</");
  const char *e = strstr(p, close);
  if (!e) return -1;
  size_t n = (size_t)(e - p);
  if (n >= out_cap) n = out_cap - 1;
  memcpy(out, p, n);
  out[n] = 0;
  return 0;
}

// Unescape minimal XML entities in-place: &amp; &lt; &gt; &quot; &apos;
static void xml_unescape_inplace(char *s) {
  char *src = s, *dst = s;
  while (*src) {
    if (*src == '&') {
      if (strncmp(src, "&amp;", 5) == 0) {
        *dst++ = '&';
        src += 5;
      } else if (strncmp(src, "&lt;", 4) == 0) {
        *dst++ = '<';
        src += 4;
      } else if (strncmp(src, "&gt;", 4) == 0) {
        *dst++ = '>';
        src += 4;
      } else if (strncmp(src, "&quot;", 6) == 0) {
        *dst++ = '"';
        src += 6;
      } else if (strncmp(src, "&apos;", 6) == 0) {
        *dst++ = '\'';
        src += 6;
      } else {
        *dst++ = *src++;
      }
    } else {
      *dst++ = *src++;
    }
  }
  *dst = 0;
}

// Extract SOAP action name from header `SOAPACTION: "urn:...#Action"`.
static int read_soap_action(httpd_req_t *req, char *out, size_t cap) {
  char buf[160];
  if (httpd_req_get_hdr_value_str(req, "SOAPACTION", buf, sizeof(buf)) != ESP_OK)
    return -1;
  // Strip optional surrounding quotes.
  char *p = buf;
  if (*p == '"') p++;
  // Action is after the #.
  char *hash = strchr(p, '#');
  if (!hash) return -1;
  p = hash + 1;
  char *q = strchr(p, '"');
  if (q) *q = 0;
  strlcpy(out, p, cap);
  return 0;
}

// Build a SOAP response envelope from a single inner-element string.
static esp_err_t soap_send(httpd_req_t *req, const char *service_urn,
                           const char *action, const char *body_args) {
  char *resp = malloc(8192);
  if (!resp) return ESP_ERR_NO_MEM;
  snprintf(resp, 8192,
           "<?xml version=\"1.0\"?>"
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
           " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
           "<s:Body><u:%sResponse xmlns:u=\"%s\">%s</u:%sResponse>"
           "</s:Body></s:Envelope>",
           action, service_urn, body_args ? body_args : "", action);
  httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
  httpd_resp_set_hdr(req, "EXT", "");
  esp_err_t rc = httpd_resp_sendstr(req, resp);
  free(resp);
  return rc;
}

static esp_err_t soap_fault(httpd_req_t *req, int code, const char *desc) {
  char resp[512];
  snprintf(resp, sizeof(resp),
           "<?xml version=\"1.0\"?>"
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
           " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
           "<s:Body><s:Fault><faultcode>s:Client</faultcode>"
           "<faultstring>UPnPError</faultstring>"
           "<detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
           "<errorCode>%d</errorCode>"
           "<errorDescription>%s</errorDescription>"
           "</UPnPError></detail></s:Fault></s:Body></s:Envelope>",
           code, desc);
  httpd_resp_set_status(req, "500 Internal Server Error");
  httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
  httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

static void secs_to_hhmmss(uint32_t secs, char *out, size_t cap) {
  uint32_t h = secs / 3600;
  uint32_t m = (secs / 60) % 60;
  uint32_t s = secs % 60;
  snprintf(out, cap, "%lu:%02lu:%02lu", (unsigned long)h, (unsigned long)m,
           (unsigned long)s);
}

static const char *transport_state_str(void) {
  switch (g_dlna.state) {
    case DLNA_STATE_STOPPED:    return "STOPPED";
    case DLNA_STATE_PLAYING:    return "PLAYING";
    case DLNA_STATE_PAUSED:     return "PAUSED_PLAYBACK";
    case DLNA_STATE_TRANSITIONING: return "TRANSITIONING";
    case DLNA_STATE_NO_MEDIA:
    default:                    return "NO_MEDIA_PRESENT";
  }
}

// ---------------------------------------------------------------------------
// AVTransport SOAP actions

static esp_err_t avt_set_uri(httpd_req_t *req, const char *body) {
  char uri[DLNA_URL_MAX] = {0};
  char metadata[DLNA_METADATA_MAX] = {0};
  xml_get_text(body, "CurrentURI", uri, sizeof(uri));
  xml_get_text(body, "CurrentURIMetaData", metadata, sizeof(metadata));
  xml_unescape_inplace(uri);
  xml_unescape_inplace(metadata);
  ESP_LOGI(TAG, "SetAVTransportURI uri=%s", uri);

  xSemaphoreTake(g_dlna.mtx, portMAX_DELAY);
  strlcpy(g_dlna.current_uri, uri, sizeof(g_dlna.current_uri));
  strlcpy(g_dlna.current_metadata, metadata, sizeof(g_dlna.current_metadata));
  g_dlna.track_title[0] = g_dlna.track_artist[0] = g_dlna.track_album[0] = 0;
  g_dlna.track_duration_secs = 0;
  g_dlna.track_position_secs = 0;
  dlna_didl_extract(metadata, g_dlna.track_title, sizeof(g_dlna.track_title),
                    g_dlna.track_artist, sizeof(g_dlna.track_artist),
                    g_dlna.track_album, sizeof(g_dlna.track_album),
                    &g_dlna.track_duration_secs);
  if (g_dlna.state != DLNA_STATE_PLAYING) g_dlna.state = DLNA_STATE_STOPPED;
  xSemaphoreGive(g_dlna.mtx);

  if (g_dlna.track_title[0])
    ESP_LOGI(TAG, "  title=%s artist=%s album=%s dur=%lus", g_dlna.track_title,
             g_dlna.track_artist, g_dlna.track_album,
             (unsigned long)g_dlna.track_duration_secs);

  return soap_send(req, "urn:schemas-upnp-org:service:AVTransport:1",
                   "SetAVTransportURI", "");
}

static esp_err_t avt_play(httpd_req_t *req, const char *body) {
  (void)body;
  ESP_LOGI(TAG, "Play");
  if (!g_dlna.current_uri[0]) {
    return soap_fault(req, 716, "Resource not found");
  }
  if (g_dlna.state == DLNA_STATE_PAUSED) {
    dlna_audio_pause(false);
    g_dlna.state = DLNA_STATE_PLAYING;
  } else if (g_dlna.state != DLNA_STATE_PLAYING) {
    g_dlna.state = DLNA_STATE_TRANSITIONING;
    dlna_audio_play_uri(g_dlna.current_uri);
  }
  return soap_send(req, "urn:schemas-upnp-org:service:AVTransport:1", "Play",
                   "");
}

static esp_err_t avt_pause(httpd_req_t *req, const char *body) {
  (void)body;
  ESP_LOGI(TAG, "Pause");
  if (g_dlna.state == DLNA_STATE_PLAYING) {
    dlna_audio_pause(true);
    g_dlna.state = DLNA_STATE_PAUSED;
  }
  return soap_send(req, "urn:schemas-upnp-org:service:AVTransport:1", "Pause",
                   "");
}

static esp_err_t avt_stop(httpd_req_t *req, const char *body) {
  (void)body;
  ESP_LOGI(TAG, "Stop");
  dlna_audio_stop();
  g_dlna.state = DLNA_STATE_STOPPED;
  return soap_send(req, "urn:schemas-upnp-org:service:AVTransport:1", "Stop",
                   "");
}

static esp_err_t avt_get_transport_info(httpd_req_t *req, const char *body) {
  (void)body;
  char args[256];
  snprintf(args, sizeof(args),
           "<CurrentTransportState>%s</CurrentTransportState>"
           "<CurrentTransportStatus>OK</CurrentTransportStatus>"
           "<CurrentSpeed>1</CurrentSpeed>",
           transport_state_str());
  return soap_send(req, "urn:schemas-upnp-org:service:AVTransport:1",
                   "GetTransportInfo", args);
}

static esp_err_t avt_get_position_info(httpd_req_t *req, const char *body) {
  (void)body;
  char dur[16], pos[16];
  secs_to_hhmmss(g_dlna.track_duration_secs, dur, sizeof(dur));
  secs_to_hhmmss(g_dlna.track_position_secs, pos, sizeof(pos));
  char *args = malloc(DLNA_METADATA_MAX + 512);
  if (!args) return ESP_ERR_NO_MEM;
  snprintf(args, DLNA_METADATA_MAX + 512,
           "<Track>1</Track>"
           "<TrackDuration>%s</TrackDuration>"
           "<TrackMetaData>%s</TrackMetaData>"
           "<TrackURI>%s</TrackURI>"
           "<RelTime>%s</RelTime>"
           "<AbsTime>%s</AbsTime>"
           "<RelCount>2147483647</RelCount>"
           "<AbsCount>2147483647</AbsCount>",
           dur, "", g_dlna.current_uri, pos, pos);
  esp_err_t rc = soap_send(req, "urn:schemas-upnp-org:service:AVTransport:1",
                           "GetPositionInfo", args);
  free(args);
  return rc;
}

static esp_err_t avt_get_media_info(httpd_req_t *req, const char *body) {
  (void)body;
  char dur[16];
  secs_to_hhmmss(g_dlna.track_duration_secs, dur, sizeof(dur));
  char *args = malloc(DLNA_METADATA_MAX + 512);
  if (!args) return ESP_ERR_NO_MEM;
  snprintf(args, DLNA_METADATA_MAX + 512,
           "<NrTracks>1</NrTracks>"
           "<MediaDuration>%s</MediaDuration>"
           "<CurrentURI>%s</CurrentURI>"
           "<CurrentURIMetaData></CurrentURIMetaData>"
           "<NextURI></NextURI>"
           "<NextURIMetaData></NextURIMetaData>"
           "<PlayMedium>NETWORK</PlayMedium>"
           "<RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
           "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
           dur, g_dlna.current_uri);
  esp_err_t rc = soap_send(req, "urn:schemas-upnp-org:service:AVTransport:1",
                           "GetMediaInfo", args);
  free(args);
  return rc;
}

// ---------------------------------------------------------------------------
// ConnectionManager SOAP

static esp_err_t cmr_get_protocol_info(httpd_req_t *req, const char *body) {
  (void)body;
  char args[1024];
  snprintf(args, sizeof(args),
           "<Source></Source>"
           "<Sink>" DLNA_SINK_PROTOCOL_INFO "</Sink>");
  return soap_send(req, "urn:schemas-upnp-org:service:ConnectionManager:1",
                   "GetProtocolInfo", args);
}

static esp_err_t cmr_get_conn_ids(httpd_req_t *req, const char *body) {
  (void)body;
  return soap_send(req, "urn:schemas-upnp-org:service:ConnectionManager:1",
                   "GetCurrentConnectionIDs",
                   "<ConnectionIDs>0</ConnectionIDs>");
}

static esp_err_t cmr_get_conn_info(httpd_req_t *req, const char *body) {
  (void)body;
  char args[512];
  snprintf(args, sizeof(args),
           "<RcsID>-1</RcsID>"
           "<AVTransportID>0</AVTransportID>"
           "<ProtocolInfo></ProtocolInfo>"
           "<PeerConnectionManager></PeerConnectionManager>"
           "<PeerConnectionID>-1</PeerConnectionID>"
           "<Direction>Input</Direction>"
           "<Status>OK</Status>");
  return soap_send(req, "urn:schemas-upnp-org:service:ConnectionManager:1",
                   "GetCurrentConnectionInfo", args);
}

// ---------------------------------------------------------------------------
// SOAP entry points

static esp_err_t read_body(httpd_req_t *req, char **out_body) {
  int len = req->content_len;
  if (len <= 0 || len > 16384) return ESP_ERR_INVALID_SIZE;
  char *buf = malloc(len + 1);
  if (!buf) return ESP_ERR_NO_MEM;
  int got = 0;
  while (got < len) {
    int n = httpd_req_recv(req, buf + got, len - got);
    if (n <= 0) {
      free(buf);
      return ESP_FAIL;
    }
    got += n;
  }
  buf[len] = 0;
  *out_body = buf;
  return ESP_OK;
}

static esp_err_t avt_ctrl_handler(httpd_req_t *req) {
  char action[64] = {0};
  if (read_soap_action(req, action, sizeof(action)) != 0) {
    return soap_fault(req, 401, "Missing SOAPACTION");
  }
  char *body = NULL;
  esp_err_t rc = read_body(req, &body);
  if (rc != ESP_OK) return soap_fault(req, 402, "Bad body");

  esp_err_t out;
  if (!strcmp(action, "SetAVTransportURI"))
    out = avt_set_uri(req, body);
  else if (!strcmp(action, "Play"))
    out = avt_play(req, body);
  else if (!strcmp(action, "Pause"))
    out = avt_pause(req, body);
  else if (!strcmp(action, "Stop"))
    out = avt_stop(req, body);
  else if (!strcmp(action, "GetTransportInfo"))
    out = avt_get_transport_info(req, body);
  else if (!strcmp(action, "GetPositionInfo"))
    out = avt_get_position_info(req, body);
  else if (!strcmp(action, "GetMediaInfo"))
    out = avt_get_media_info(req, body);
  else
    out = soap_fault(req, 401, "Action not implemented");

  free(body);
  return out;
}

static esp_err_t cmr_ctrl_handler(httpd_req_t *req) {
  char action[64] = {0};
  if (read_soap_action(req, action, sizeof(action)) != 0) {
    return soap_fault(req, 401, "Missing SOAPACTION");
  }
  char *body = NULL;
  esp_err_t rc = read_body(req, &body);
  if (rc != ESP_OK) return soap_fault(req, 402, "Bad body");

  esp_err_t out;
  if (!strcmp(action, "GetProtocolInfo"))
    out = cmr_get_protocol_info(req, body);
  else if (!strcmp(action, "GetCurrentConnectionIDs"))
    out = cmr_get_conn_ids(req, body);
  else if (!strcmp(action, "GetCurrentConnectionInfo"))
    out = cmr_get_conn_info(req, body);
  else
    out = soap_fault(req, 401, "Action not implemented");

  free(body);
  return out;
}

// GENA: stub-only. Return 200 with a fake SID; we never actually notify.
// Control points poll GetPositionInfo for the progress bar.
static esp_err_t gena_subscribe_handler(httpd_req_t *req) {
  char sid[64];
  uint32_t r = esp_random();
  snprintf(sid, sizeof(sid), "uuid:%08lx-0000-0000-0000-000000000000",
           (unsigned long)r);
  httpd_resp_set_hdr(req, "SID", sid);
  httpd_resp_set_hdr(req, "TIMEOUT", "Second-1800");
  httpd_resp_set_status(req, "200 OK");
  httpd_resp_send(req, NULL, 0);
  return ESP_OK;
}

esp_err_t dlna_http_register(void) {
  httpd_uri_t desc = {.uri = "/dlna/desc.xml", .method = HTTP_GET,
                      .handler = serve_root_desc};
  httpd_uri_t avt = {.uri = "/dlna/AVTransport.xml", .method = HTTP_GET,
                     .handler = serve_avt_scpd};
  httpd_uri_t cmr = {.uri = "/dlna/ConnectionManager.xml", .method = HTTP_GET,
                     .handler = serve_cmr_scpd};
  httpd_uri_t avtc = {.uri = "/dlna/AVTransport/ctrl", .method = HTTP_POST,
                      .handler = avt_ctrl_handler};
  httpd_uri_t cmrc = {.uri = "/dlna/ConnectionManager/ctrl",
                      .method = HTTP_POST, .handler = cmr_ctrl_handler};
  // SUBSCRIBE is HTTP method 9 in esp_http_server. Some control points also
  // send UNSUBSCRIBE; we accept both via a single stub.
  httpd_uri_t avte = {.uri = "/dlna/AVTransport/evt",
                      .method = (httpd_method_t)9, .handler = gena_subscribe_handler};
  httpd_uri_t cmre = {.uri = "/dlna/ConnectionManager/evt",
                      .method = (httpd_method_t)9, .handler = gena_subscribe_handler};

  esp_err_t rc;
  if ((rc = httpd_register_uri_handler(g_dlna.httpd, &desc)) != ESP_OK ||
      (rc = httpd_register_uri_handler(g_dlna.httpd, &avt)) != ESP_OK ||
      (rc = httpd_register_uri_handler(g_dlna.httpd, &cmr)) != ESP_OK ||
      (rc = httpd_register_uri_handler(g_dlna.httpd, &avtc)) != ESP_OK ||
      (rc = httpd_register_uri_handler(g_dlna.httpd, &cmrc)) != ESP_OK) {
    ESP_LOGE(TAG, "uri register failed: %s", esp_err_to_name(rc));
    return rc;
  }
  // Eventing handlers — register only if SUBSCRIBE method is supported.
  (void)httpd_register_uri_handler(g_dlna.httpd, &avte);
  (void)httpd_register_uri_handler(g_dlna.httpd, &cmre);

  ESP_LOGI(TAG, "HTTP endpoints registered");
  return ESP_OK;
}
