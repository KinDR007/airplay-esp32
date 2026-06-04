# SKILLS — recipes for common tasks on airplay-esp32

Short, copy-pasteable workflows. Read [CLAUDE.md](CLAUDE.md) first for project
context.

---

## Build + flash + monitor (ESP32-P4 / Guition)

```bash
# 1. Rebuild
pio run -e esp32p4

# 2. Close any open monitor (Ctrl+C) — the port is exclusive.

# 3. Flash firmware
pio run -e esp32p4 -t upload --upload-port /dev/cu.usbserial-2120

# 4. (Optional) Flash SPIFFS image (web UI, DAC firmware, display assets)
pio run -e esp32p4 -t uploadfs --upload-port /dev/cu.usbserial-2120

# 5. Monitor
pio device monitor -e esp32p4 --port /dev/cu.usbserial-2120
```

Port detection: `pio device list` — look for `USB VID:PID=1A86:7523` (CH340).

---

## Force sdkconfig regen after editing defaults

PIO caches the merged sdkconfig per environment. After changes to
`sdkconfig.defaults*`:

```bash
rm sdkconfig.esp32p4
pio run -e esp32p4
```

---

## Debug Spotify Connect

1. Check the device shows up in mDNS:
   ```bash
   dns-sd -B _spotify-connect._tcp local
   ```
   Should list the device name. If not, mDNS registration failed — verify
   `bell::MDNSService::registerService` ran in the boot log.

2. Verify ZeroConf endpoint:
   ```bash
   curl http://<device-ip>/spotify_info | jq .
   ```
   Should return a JSON blob with `deviceID`, `publicKey`, `version`, etc.
   Empty or 404 → cspot didn't register handlers (check
   `web_server_get_handle()` returned non-NULL at boot).

3. **"Premium required" in the Spotify app is expected** — that's a Spotify
   policy, not a port issue. Use a Premium account (trial OK) to actually
   stream. Even with Premium, authentication needs valid
   `CLIENT_ID`/`CLIENT_SECRET`:

   ```bash
   export SPOTIFY_SECRET='-DCLIENT_ID=\"xxx\" -DCLIENT_SECRET=\"yyy\"'
   pio run -e esp32p4 -t upload
   ```

4. Boot log markers to look for:
   ```
   I (xxxxx) main: Initializing Spotify Connect...
   I (xxxxx) main:   name=<device> srv=0x<addr> port=80
   I (xxxxx) spotify_connect: cspot ready: name=<...> id=<...>
   I (xxxxx) spotify_connect: ZeroConf registered on /spotify_info (port 80)
   I (xxxxx) main: Spotify Connect ready (port 80)
   ```
   Missing → see "Spotify Connect didn't start" below.

---

## Spotify Connect didn't start

If the boot log skips straight from `mdns_airplay_init` to `rtsp_server`
without the `Initializing Spotify Connect...` line:

- Verify `CONFIG_SPOTIFY_CONNECT_ENABLE=1` in the generated
  `.pio/build/esp32p4/config/sdkconfig.h`. If missing, delete
  `sdkconfig.esp32p4` and rebuild.
- Confirm the symbol exists:
  `nm .pio/build/esp32p4/firmware.elf | grep spotify_connect_init`
- Confirm `main.c.o` references it:
  `riscv32-esp-elf-objdump -t .pio/build/esp32p4/main/main.c.o | grep spotify`

---

## Coexistence with AirPlay / Bluetooth

The active source is held in `playback_control_get_source()`. Switching is
driven by:

- AirPlay → set on `start_airplay_services()` (main.c).
- Bluetooth → set on `on_bt_state_changed()` (main.c, only with
  `CONFIG_BT_A2DP_ENABLE`).
- Spotify → set inside `cspotPlayer::pcmWrite()` on first PCM of a session,
  released on session end (spotify_connect.cpp).

When a new source claims output it calls `audio_output_stop()` +
`audio_output_flush()` first, then `audio_output_set_sample_rate()` +
`audio_output_start()`. The previous source's PCM writes will then block /
return ESP_FAIL until that source reclaims.

---

## Web UI / OTA

- Web UI files live in `data/www/` and ship in the SPIFFS image.
- Hit `http://<device-ip>/` for the config page (Wi-Fi creds, OTA, EQ,
  speedtest, log stream).
- OTA: POST to `/api/ota/upload` with the new `.bin`. Implementation in
  `main/network/ota.c`.

---

## Adding a new HTTP endpoint

```c
// In your component (or main):
#include "esp_http_server.h"
#include "web_server.h"   // for web_server_get_handle()

static esp_err_t my_handler(httpd_req_t *req) { ... }

void my_init(void) {
  httpd_handle_t s = web_server_get_handle();
  if (!s) return;
  httpd_uri_t u = {"/my/path", HTTP_GET, my_handler, NULL};
  httpd_register_uri_handler(s, &u);
}
```

If `max_uri_handlers` overflows, bump it in
`main/network/web_server.c:web_server_start()`.

---

## Adding a new mDNS service

```c
#include "mdns.h"
mdns_txt_item_t txt[] = {{"key","val"}};
mdns_service_add("My Device", "_my-svc", "_tcp", 1234, txt, 1);
```

Multiple services coexist on the single `mdns_init()` from
`mdns_airplay_init()`. Don't call `mdns_init()` again.

---

## Getting Spotify Commercial Hardware credentials

cspot needs a `CLIENT_ID` + `CLIENT_SECRET` issued for the
**Commercial Hardware** program — these are not the same as the regular
developer-app credentials.

1. Sign in at https://developer.spotify.com/dashboard with a Spotify account.
2. Create an app (any name). This gives you a regular Client ID / Secret —
   **these alone are insufficient** for Spotify Connect.
3. Apply for the [Commercial Hardware program](https://developer.spotify.com/documentation/commercial-hardware).
   - Fill the form: device type, expected number of units, intended use.
   - For personal / hobbyist use, state "personal hardware project — not for
     resale" — review can take days to weeks; approval is required for OAuth
     auth (`Session::authenticate` to return non-empty `authData`).
4. Once approved, your app dashboard gets a Connect-enabled Client ID + Secret.
5. Inject at build time:
   ```bash
   export SPOTIFY_SECRET='-DCLIENT_ID=\"abc123...\" -DCLIENT_SECRET=\"def456...\"'
   pio run -e esp32p4 -t upload
   ```
   The `-D...` flags are passed to the compiler for `Shim.cpp` (squeezelite)
   and `spotify_connect.cpp` (ours) via `set_source_files_properties` in
   [components/cspot/CMakeLists.txt](components/cspot/CMakeLists.txt).
6. Verify in boot log: after `Connecting to Spotify AP...` you should see
   the cspot mercury log "AuthChallenges: authenticated successfully". Empty
   `authData` → wrong/missing credentials.

**Without Commercial Hardware credentials**, ZeroConf still works (device
shows up in the picker, GET `/spotify_info` responds) but the moment a
Premium user clicks Connect, `authenticate` returns empty and the device
re-registers ZeroConf in a loop.

---

## Non-ZeroConf (keepalive) mode

By default the device only authenticates with Spotify when a controller
clicks "Connect" in the app. In **keepalive** mode it authenticates at boot
and keeps the Mercury session alive forever, so it stays visible in:
- Other devices' Spotify pickers (not just the LAN it's on)
- Spotify WebAPI (`GET /me/player/devices`)
- Home Assistant `media_player.spotify` integration
- Spotify Desktop on a different network

**Tradeoffs**: ~30–50 KB more RAM (Mercury session always live), constant
Spotify traffic, and once-per-day token refresh from Spotify servers.

### Enable / disable at runtime via REST

```bash
# Status — what mode are we in?
curl http://<device-ip>/api/spotify/status
# {"deviceName":"airplay-esp32","keepalive":false,"haveCredentials":true,
#  "linked":false,"active":false}

# Turn keepalive on (needs creds stored — see flow below)
curl -X POST http://<device-ip>/api/spotify/keepalive \
     -H "Content-Type: application/json" \
     -d '{"enabled":true}'

# Turn it off
curl -X POST http://<device-ip>/api/spotify/keepalive -d '{"enabled":false}'

# Wipe credentials (also disables keepalive)
curl -X POST http://<device-ip>/api/spotify/forget
```

Or programmatically from C: `spotify_connect_set_keepalive(true)`,
`spotify_connect_get_keepalive()`, `spotify_connect_has_credentials()`,
`spotify_connect_forget_credentials()`.

### First-time flow

1. Fresh boot → keepalive is OFF, no credentials → device in pure ZeroConf
   mode (waiting for a click).
2. Open Spotify app → pick the device → it pairs.
3. cspot saves credentials to NVS (`spotify/credentials`).
4. Enable keepalive: `POST /api/spotify/keepalive {"enabled":true}`.
5. The toggle takes effect at the **next session boundary** (immediately if
   no client is currently connected; otherwise after the current session
   ends).
6. From now on, every boot brings up an active Mercury session.

### Forced fallback

If stored credentials become invalid (Spotify password change, token
revoked), `authenticate` returns empty → shim auto-falls-back to ZeroConf
for that iteration and waits for the user to re-pair. No reboot needed.

### Failure modes

| Symptom | Cause |
|---|---|
| Boot log: `Waiting for ZeroConf connect...` even with keepalive on | No `credentials` blob in NVS yet — pair once via app first |
| Boot log: `auth failed` after `Connecting to Spotify AP...` | Stored creds expired/revoked, OR no SPOTIFY_SECRET at build time |
| `POST /api/spotify/keepalive` returns 500 | `g_player` not initialized — check `spotify_connect_init` ran |
| Loops between `connecting` and `auth failed` | Spotify rejected the device. Wipe with `/forget` and start over. |

---

## NVS keys cspot expects

All in NVS namespace **`spotify`** (separate from `airplay`).

| Key | Type | Set by | Read by | Meaning |
|---|---|---|---|---|
| `credentials` | str | cspot (after first successful auth) | shim on boot | JSON blob: `authType`, `authData`, `username`, `deviceId` |
| `keepalive` | u8 | REST `/api/spotify/keepalive` or `spotify_connect_set_keepalive()` | shim on boot + at end of each session | 0 = ZeroConf-only, 1 = auto-reconnect each iteration |

Squeezelite-esp32 also uses a `cspot_config` JSON blob (in `platform_config`
namespace) with `volume`, `bitrate`, `deviceName`, `zeroConf` — we don't,
because:
- volume is owned by `airplay`/DAC settings
- bitrate is build-time via `CONFIG_SPOTIFY_CONNECT_BITRATE`
- deviceName comes from `settings_get_device_name()`
- zeroConf is replaced by our `keepalive` flag (inverted)

To inspect / wipe manually:
```bash
# Over USB-serial, in monitor (esp_console)
nvs-set spotify keepalive u8 1
nvs-erase spotify credentials
# Or via menuconfig: enable CONFIG_NVS_CLI=y first.
```

---

## Monitoring cspot logs

The bell library has its own logger (`bell::setDefaultLogger`). On ESP32 it
forwards to `printf`, which goes to the same UART as ESP-IDF logs. Tags to
grep for in `pio device monitor`:

| Source | Tag | Notes |
|---|---|---|
| Our shim | `spotify_connect` | High-level state (ZeroConf wait, auth, session end) |
| bell::Logger | (no tag — bare printf with timestamp) | Low-level: Mercury packets, SPIRC events, TrackPlayer fills |
| cspot Session | `Session` | Connection-level events |
| cspot SpircHandler | `Spirc` | Volume, play/pause, track changes |
| cspot TrackPlayer | `TrackPlayer` | Vorbis decode + PCM fill |
| esp_http_server | `httpd_*` | Incoming /spotify_info and /api/spotify calls |
| espressif__mdns | `mdns` / `MDNS` | Service registration |

Useful filters:
```bash
# Just our shim
pio device monitor -e esp32p4 --port /dev/cu.usbserial-2120 \
    --filter direct 2>&1 | grep -E "spotify_connect|Spirc|TrackPlayer"

# Everything except noisy esp_hosted heartbeat
pio device monitor -e esp32p4 --port /dev/cu.usbserial-2120 2>&1 \
    | grep -v "H_SDIO_DRV\|RPC_WRAP"
```

To bump verbosity at runtime, increase `CONFIG_LOG_MAXIMUM_LEVEL` to
`VERBOSE` in menuconfig (then `esp_log_level_set("spotify_connect",
ESP_LOG_VERBOSE)` in main.c if you want per-tag).

To capture a session for debugging:
```bash
pio device monitor -e esp32p4 --port /dev/cu.usbserial-2120 \
    --filter direct --filter log2file 2>&1 | tee /tmp/cspot-session.log
```

The cspot logger does NOT respect `CONFIG_LOG_MAXIMUM_LEVEL` — to silence
it you'd have to subclass `bell::AbstractLogger`. The default
`enableTimestampLogging(true)` we set in `spotify_connect_init` keeps lines
correlated with ESP-IDF log timestamps.

---

## AirPlay 2 metadata pipeline

In AP2 buffered audio mode (stream type 103, used by Apple Music / Spotify
via iOS AirPlay route), iOS does NOT use the classic AP1 `SET_PARAMETER` +
`application/x-dmap-tagged` path. Instead it streams **MediaRemote NowPlayingInfo**
bplist payloads via `POST /command` on the same RTSP TCP control socket.

### Required prerequisites (gating the feature)

iPhone only sends metadata if the receiver advertises specific feature bits
in mDNS TXT. See [main/rtsp/rtsp_handlers.h](main/rtsp/rtsp_handlers.h):
- bit 15 — `AudioMetaCovers` (artwork)
- bit 16 — `AudioMetaProgress` (elapsed/duration)
- bit 17 — `AudioMetaTxtDAAP` (DMAP tag fallback)
- bit 29 — `plistMetaData` (enables bplist meta channel)
- bit 50 — `NowPlayingInfo` (MediaRemote payloads)

Without these, iPhone treats the receiver as a dumb sink and sends only audio.

### How it works

```
iPhone --POST /command--> rtsp_server (port 7000)
  Content-Type: application/x-apple-binary-plist
  Body: bplist00 { params: { params: {
          kMRMediaRemoteNowPlayingInfoTitle    = "..."
          kMRMediaRemoteNowPlayingInfoArtist   = "..."
          kMRMediaRemoteNowPlayingInfoAlbum    = "..."
          kMRMediaRemoteNowPlayingInfoGenre    = "..."
          kMRMediaRemoteNowPlayingInfoDuration = 379.0
          kMRMediaRemoteNowPlayingInfoElapsedTime = 12.3
          kMRMediaRemoteNowPlayingInfoArtworkData = <JPEG bytes>
          kMRMediaRemoteNowPlayingInfoArtworkMIMEType = "image/jpeg"
        } } }
```

The handler in [rtsp_handlers.c:699](main/rtsp/rtsp_handlers.c#L699) calls
`bplist_find_string_deep` / `bplist_find_real_deep` (defined in
[main/plist/bplist_parser.c](main/plist/bplist_parser.c) — recursive walk that
descends into nested dicts/arrays up to depth 10) for each key, populates a
`rtsp_metadata_t`, and emits `RTSP_EVENT_METADATA`.

The `now_playing` listener in [main.c](main/main.c) consumes it and logs:
```
I (xxxxx) now_playing: NOW PLAYING: The Rolling Stones - Sympathy For The Devil [Beggars Banquet (2018 Remaster)] (6:19)
I (xxxxx) now_playing:   genre: Rock
```

### What iPhone sends (observed body sizes)

| ~Size | Content |
|---|---|
| 64–200 B | command headers, play/pause hints |
| 300–500 B | small NowPlayingInfo update (progress only) |
| 1.5–6 kB | full NowPlayingInfo without artwork |
| 40–200 kB | NowPlayingInfo + ArtworkData JPEG |
| 0 B (no Content-Type) | heartbeat keepalive |

We only emit on payloads carrying `Title` or `Artist` — progress-only and
heartbeat updates are silently 200-OK'd to avoid flooding the log.

### AP1 fallback (CONFIG_AIRPLAY_FORCE_V1)

If you force AP1 mode, iOS uses classic RAOP and sends DMAP-tagged metadata
via `SET_PARAMETER` instead. Both code paths exist and feed the same
`RTSP_EVENT_METADATA` event — the now_playing listener doesn't care.

### Artwork (not surfaced yet)

`ArtworkData` arrives as a base64-decoded JPEG inside the bplist. Currently
we don't extract it — it would need `bplist_find_data_deep` (which already
exists) + somewhere to put a ~50 kB buffer. Hook point: same handler in
`rtsp_handlers.c`, just add `bplist_find_data_deep(..., "kMRMediaRemoteNowPlayingInfoArtworkData", ...)`
and pipe it into the display or web UI.

### Sources / references

- openairplay/airplay2-receiver (Python ref): `ap2-receiver.py:885-905`,
  `ap2/dxxp.py` (DMAP codes), `ap2/bitflags.py` (feature bit meanings)
- shairport-sync `rtsp.c` — confirmed via code review that even in AP2 mode
  it only parses `SET_PARAMETER` (and is therefore blind to NowPlayingInfo
  from iOS 16+). Our handler is more complete than upstream shairport-sync.

---

## Patches applied to vendored cspot/bell

If you ever re-vendor from upstream squeezelite-esp32, re-apply these:

1. `cspot_lib/CMakeLists.txt:1` — `cmake_minimum_required 2.8.12 → 3.16`.
2. `cspot_lib/bell/CMakeLists.txt:1` — same.
3. `cspot_lib/bell/CMakeLists.txt:~108` — guard `biquad_f32_ae32.S` behind
   `CONFIG_IDF_TARGET_ARCH_XTENSA`.
4. `cspot_lib/bell/main/io/URLParser.cpp:1` — add `#include <cstring>`.
5. `cspot_lib/bell/external/nanopb/generator/nanopb_generator.py:1466` —
   try/except fallback to `message_factory.GetMessageClass` for protobuf ≥5.

These keep building broken on toolchains that ship CMake 4.x, GCC 14+, and
protobuf 5+.
