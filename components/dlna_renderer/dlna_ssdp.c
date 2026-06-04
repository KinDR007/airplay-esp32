// SSDP daemon — UDP multicast 239.255.255.250:1900.
//
//  - Joins multicast group on the STA interface.
//  - Sends NOTIFY ssdp:alive for 6 NT/USN records at startup (3× spaced 100 ms),
//    then re-advertises every 900 s (max-age/2).
//  - Responds to M-SEARCH requests (unicast, after random 0..MX seconds delay).
//  - Sends ssdp:byebye on shutdown.
//
// Port shape inspired by Movian's ssdp.c (BSD) and uSSDP-ESP32 (MIT).

#include "dlna_internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sys/socket.h"

static const char *TAG = "dlna_ssdp";

#define SSDP_MCAST_ADDR "239.255.255.250"
#define SSDP_MCAST_PORT 1900
#define SSDP_MAX_AGE    1800
#define SSDP_REANNOUNCE_PERIOD_SECS  900
#define SSDP_SERVER_HDR "FreeRTOS/10 UPnP/1.0 esp32-airplay/1.0"

typedef struct {
  const char *nt;        // NT header value
  const char *usn_suffix; // "" or "::<urn-or-rootdevice>"
} ssdp_record_t;

static const ssdp_record_t SSDP_RECORDS[] = {
    {"upnp:rootdevice", "::upnp:rootdevice"},
    {NULL, ""},  // NT == USN == uuid — filled in at runtime
    {"urn:schemas-upnp-org:device:MediaRenderer:1",
     "::urn:schemas-upnp-org:device:MediaRenderer:1"},
    {"urn:schemas-upnp-org:service:AVTransport:1",
     "::urn:schemas-upnp-org:service:AVTransport:1"},
    {"urn:schemas-upnp-org:service:ConnectionManager:1",
     "::urn:schemas-upnp-org:service:ConnectionManager:1"},
};
#define N_RECORDS (sizeof(SSDP_RECORDS) / sizeof(SSDP_RECORDS[0]))

static void build_alive(char *out, size_t cap, const ssdp_record_t *rec) {
  const char *nt = rec->nt ? rec->nt : g_dlna.udn;
  char usn[220];
  snprintf(usn, sizeof(usn), "%s%s", g_dlna.udn, rec->usn_suffix);
  snprintf(out, cap,
           "NOTIFY * HTTP/1.1\r\n"
           "HOST: %s:%d\r\n"
           "CACHE-CONTROL: max-age=%d\r\n"
           "LOCATION: http://%s:%u/dlna/desc.xml\r\n"
           "NT: %s\r\n"
           "NTS: ssdp:alive\r\n"
           "SERVER: %s\r\n"
           "USN: %s\r\n"
           "\r\n",
           SSDP_MCAST_ADDR, SSDP_MCAST_PORT, SSDP_MAX_AGE, g_dlna.local_ip,
           g_dlna.http_port, nt, SSDP_SERVER_HDR, usn);
}

static void build_byebye(char *out, size_t cap, const ssdp_record_t *rec) {
  const char *nt = rec->nt ? rec->nt : g_dlna.udn;
  char usn[220];
  snprintf(usn, sizeof(usn), "%s%s", g_dlna.udn, rec->usn_suffix);
  snprintf(out, cap,
           "NOTIFY * HTTP/1.1\r\n"
           "HOST: %s:%d\r\n"
           "NT: %s\r\n"
           "NTS: ssdp:byebye\r\n"
           "USN: %s\r\n"
           "\r\n",
           SSDP_MCAST_ADDR, SSDP_MCAST_PORT, nt, usn);
}

static void build_msearch_resp(char *out, size_t cap, const char *st,
                               const char *usn) {
  // Date in RFC 1123 form; clients accept any sensible date.
  time_t now = time(NULL);
  struct tm t;
  gmtime_r(&now, &t);
  char date[40];
  strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &t);
  snprintf(out, cap,
           "HTTP/1.1 200 OK\r\n"
           "CACHE-CONTROL: max-age=%d\r\n"
           "DATE: %s\r\n"
           "EXT:\r\n"
           "LOCATION: http://%s:%u/dlna/desc.xml\r\n"
           "SERVER: %s\r\n"
           "ST: %s\r\n"
           "USN: %s\r\n"
           "\r\n",
           SSDP_MAX_AGE, date, g_dlna.local_ip, g_dlna.http_port,
           SSDP_SERVER_HDR, st, usn);
}

static void send_multicast(int sock, const char *buf) {
  struct sockaddr_in dst = {0};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(SSDP_MCAST_PORT);
  inet_aton(SSDP_MCAST_ADDR, &dst.sin_addr);
  sendto(sock, buf, strlen(buf), 0, (struct sockaddr *)&dst, sizeof(dst));
}

static void announce_alive_burst(int sock) {
  char buf[768];
  for (int round = 0; round < 3; round++) {
    for (size_t i = 0; i < N_RECORDS; i++) {
      build_alive(buf, sizeof(buf), &SSDP_RECORDS[i]);
      send_multicast(sock, buf);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  ESP_LOGI(TAG, "alive sent (3 rounds × %u records)", (unsigned)N_RECORDS);
}

static void announce_byebye(int sock) {
  char buf[768];
  for (size_t i = 0; i < N_RECORDS; i++) {
    build_byebye(buf, sizeof(buf), &SSDP_RECORDS[i]);
    send_multicast(sock, buf);
  }
}

// Case-insensitive prefix match (for header parsing).
static bool starts_with_ci(const char *str, const char *pre) {
  return strncasecmp(str, pre, strlen(pre)) == 0;
}

// Match incoming ST against our records. Returns matching record index, or
// -1 if no match. Special: ssdp:all matches everything.
static void handle_msearch(int sock, struct sockaddr_in *from, const char *st,
                           int mx) {
  // Random 0..MX-1 second delay (UDA requirement).
  if (mx > 0) {
    int delay_ms = esp_random() % (mx * 1000);
    if (delay_ms > 3000) delay_ms = 3000;  // sanity cap
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }

  bool ssdp_all = strcmp(st, "ssdp:all") == 0;
  char resp[700];
  char usn[220];

  for (size_t i = 0; i < N_RECORDS; i++) {
    const char *nt = SSDP_RECORDS[i].nt ? SSDP_RECORDS[i].nt : g_dlna.udn;
    bool match = ssdp_all || strcmp(st, nt) == 0;
    if (!match) continue;
    snprintf(usn, sizeof(usn), "%s%s", g_dlna.udn, SSDP_RECORDS[i].usn_suffix);
    build_msearch_resp(resp, sizeof(resp), nt, usn);
    sendto(sock, resp, strlen(resp), 0, (struct sockaddr *)from, sizeof(*from));
    if (!ssdp_all) return;  // M-SEARCH for one ST → one response only
  }
}

static void ssdp_task(void *arg) {
  (void)arg;

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (sock < 0) {
    ESP_LOGE(TAG, "socket() failed");
    vTaskDelete(NULL);
    return;
  }
  int yes = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in saddr = {0};
  saddr.sin_family = AF_INET;
  saddr.sin_port = htons(SSDP_MCAST_PORT);
  saddr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
    ESP_LOGE(TAG, "bind failed");
    close(sock);
    vTaskDelete(NULL);
    return;
  }

  // Join multicast group on our interface.
  struct ip_mreq mreq = {0};
  inet_aton(SSDP_MCAST_ADDR, &mreq.imr_multiaddr.s_addr);
  inet_aton(g_dlna.local_ip, &mreq.imr_interface.s_addr);
  if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) <
      0) {
    ESP_LOGW(TAG, "IP_ADD_MEMBERSHIP failed");
  }
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &mreq.imr_interface,
             sizeof(struct in_addr));
  uint8_t ttl = 4;
  setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

  g_dlna.ssdp_sock = sock;
  announce_alive_burst(sock);

  time_t last_announce = time(NULL);
  char buf[1500];
  while (!g_dlna.ssdp_stop) {
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(sock, &rfds);
    int rc = select(sock + 1, &rfds, NULL, NULL, &tv);
    if (rc > 0 && FD_ISSET(sock, &rfds)) {
      int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                       (struct sockaddr *)&from, &flen);
      if (n > 0) {
        buf[n] = 0;
        // Quick parse: M-SEARCH * HTTP/1.1 + headers
        if (starts_with_ci(buf, "M-SEARCH")) {
          char st[160] = {0};
          int mx = 1;
          char *line = strtok(buf, "\r\n");
          while (line) {
            if (starts_with_ci(line, "ST:")) {
              const char *v = line + 3;
              while (*v == ' ') v++;
              strlcpy(st, v, sizeof(st));
            } else if (starts_with_ci(line, "MX:")) {
              mx = atoi(line + 3);
              if (mx < 1) mx = 1;
              if (mx > 5) mx = 5;
            }
            line = strtok(NULL, "\r\n");
          }
          if (st[0]) handle_msearch(sock, &from, st, mx);
        }
      }
    }

    time_t now = time(NULL);
    if (now - last_announce > SSDP_REANNOUNCE_PERIOD_SECS) {
      announce_alive_burst(sock);
      last_announce = now;
    }
  }

  announce_byebye(sock);
  close(sock);
  g_dlna.ssdp_sock = -1;
  ESP_LOGI(TAG, "ssdp task exiting");
  vTaskDelete(NULL);
}

esp_err_t dlna_ssdp_start(void) {
  g_dlna.ssdp_stop = false;
  if (xTaskCreatePinnedToCore(ssdp_task, "dlna_ssdp", 5120, NULL, 5,
                              &g_dlna.ssdp_task, 0) != pdPASS) {
    ESP_LOGE(TAG, "ssdp task spawn failed");
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

void dlna_ssdp_stop(void) {
  if (!g_dlna.ssdp_task) return;
  g_dlna.ssdp_stop = true;
  // Task will exit on next select() timeout (≤2 s).
  for (int i = 0; i < 30 && g_dlna.ssdp_task; i++) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
