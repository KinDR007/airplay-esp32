# airplay-esp32 — project context for Claude Code

AirPlay 2 receiver for ESP32 (S3, P4, classic) with optional Spotify Connect
and Bluetooth A2DP. Built with PlatformIO + ESP-IDF 5.5.

## Quick build / flash

```bash
# Build for ESP32-P4 (Guition / Waveshare boards, esp_hosted Wi-Fi via C6)
pio run -e esp32p4

# Flash firmware (CH340 enumerates as /dev/cu.usbserial-2120 on this Mac)
pio run -e esp32p4 -t upload --upload-port /dev/cu.usbserial-2120

# Flash SPIFFS image too (web UI assets, DAC firmware, display bg)
pio run -e esp32p4 -t uploadfs --upload-port /dev/cu.usbserial-2120

# Serial monitor
pio device monitor -e esp32p4 --port /dev/cu.usbserial-2120
```

Other envs in `platformio.ini`: `squeezeamp`, `esp32s3`, `esparagus-*`, `wrover`, etc.

## Layout

```
main/                  — application (C). Entry: app_main() in main.c.
  audio/               — RTSP/RAOP audio pipeline (receive → decode → output)
  rtsp/                — AirPlay 2 RTSP server + FairPlay
  hap/                 — HomeKit Accessory Protocol (pairing)
  network/             — wifi/eth/mdns/web_server/ota/dns
  playback_control.c   — source-agnostic transport (AirPlay / BT / Spotify)
  settings.c           — NVS settings (namespace "airplay")

components/
  cspot/               — vendored cspot + bell from squeezelite-esp32 (Spotify)
  spotify_connect/     — our shim that bridges cspot to audio_output / web_server
  boards/              — per-board pin maps (Squeezeamp, Waveshare P4, Guition…)
  dac/, dac_tas58xx/   — DAC drivers
  display/, u8g2*/     — optional ST7789/SSD1306
  audio-resampler/     — ESP-DSP-based 44.1↔48k resampler
  spiffs_storage/      — SPIFFS init
```

## Key integration points

- **Audio output API** (`main/audio/audio_output.h`): any source feeds
  `audio_output_write(pcm, bytes, wait)`. Stereo 16-bit interleaved.
  Call `audio_output_stop()` first to take over from AirPlay.
- **Web server** (`main/network/web_server.c`): single shared `httpd_handle_t`.
  Get it with `web_server_get_handle()` (added for Spotify Connect).
- **mDNS**: AirPlay services live in `main/network/mdns_airplay.c`. Spotify
  Connect registers `_spotify-connect._tcp` via `bell::MDNSService` from inside
  the cspot shim — both coexist on the same `mdns_init()`.
- **AirPlay 2 metadata**: in buffered audio mode (stream type 103) iOS does
  NOT send DMAP via `SET_PARAMETER`. Instead it posts MediaRemote NowPlayingInfo
  bplist payloads to `POST /command` on the same RTSP TCP socket. Handled in
  [rtsp_handlers.c:699](main/rtsp/rtsp_handlers.c#L699) using
  `bplist_find_string_deep` / `bplist_find_real_deep`. Triggers require feature
  bits 15/16/17/29/50 set in `AIRPLAY_FEATURES_LO/HI` (see
  [rtsp_handlers.h](main/rtsp/rtsp_handlers.h)). Emits `RTSP_EVENT_METADATA`
  consumed by `now_playing` listener in `main.c`.
- **NVS namespaces**: `airplay` (settings.c), `spotify` (cspot credentials).
- **Playback source** (`main/playback_control.h`): `PLAYBACK_SOURCE_NONE /
  AIRPLAY / BLUETOOTH / SPOTIFY`. Sources mutually exclude each other.

## Spotify Connect (cspot)

Vendored from squeezelite-esp32 into [components/cspot/cspot_lib/](components/cspot/cspot_lib/).
Wrapped by our own CMakeLists; shim is in [components/spotify_connect/](components/spotify_connect/).

Two modes (toggled at runtime via `POST /api/spotify/keepalive {"enabled":bool}`):
- **ZeroConf** (default): device shows up only when discovered on the same LAN;
  cspot session sleeps between connects → less RAM.
- **Keepalive** (non-ZeroConf): if credentials are stored in NVS, cspot
  authenticates at boot and stays online → visible to Spotify WebAPI,
  Home Assistant, and other Spotify clients on different networks. Uses
  ~30–50 KB more RAM. See SKILLS.md for the flow.

The device appears in the Spotify app's device list but **Spotify Premium
is required** for any account to actually stream to it (that's a Spotify
policy, not a port limitation).

For real authentication you must build with Spotify Commercial Hardware
credentials:
```bash
export SPOTIFY_SECRET='-DCLIENT_ID=\"xxx\" -DCLIENT_SECRET=\"yyy\"'
pio run -e esp32p4 -t upload
```
Without them the GET `/spotify_info` handshake works (device shows up in app)
but `Session::authenticate` returns empty authData and the cspot loop just
re-registers ZeroConf.

Enable / disable via Kconfig: `CONFIG_SPOTIFY_CONNECT_ENABLE` (default y).

## Project-specific gotchas

- **C++20 required globally** — set in root `CMakeLists.txt` before
  `project()`. cspot + bell use C++20 features.
- **C++ exceptions must be on** — `CONFIG_COMPILER_CXX_EXCEPTIONS=y` in
  `sdkconfig.defaults.esp32p4`. bell throws `std::invalid_argument` on bad URLs.
- **`CMAKE_POLICY_VERSION_MINIMUM 3.5`** is set in
  [components/cspot/CMakeLists.txt](components/cspot/CMakeLists.txt) so the
  CMake-2.x policies inside `cspot_lib/` + `bell/external/*` still resolve.
- **Vendored nanopb is patched** for protobuf ≥5 (`reflection.MakeClass` →
  `message_factory.GetMessageClass`). See
  [components/cspot/cspot_lib/bell/external/nanopb/generator/nanopb_generator.py:1466](components/cspot/cspot_lib/bell/external/nanopb/generator/nanopb_generator.py#L1466).
- **`biquad_f32_ae32.S` is Xtensa-only** — wrapped in
  `if(CONFIG_IDF_TARGET_ARCH_XTENSA)` in `bell/CMakeLists.txt` so RISC-V (P4/C3/C6)
  builds don't break.
- **ESP32-P4 Wi-Fi** comes from an onboard ESP32-C6 over SDIO (esp_hosted).
  All Wi-Fi config in `sdkconfig.defaults.esp32p4`. Don't bother with
  `esp_wifi_init()` — it's wrapped.
- **`espressif/mdns` managed component** is required (lockfile already has it);
  bell links against `idf::espressif__mdns` on IDF ≥5.
- **web_server `max_uri_handlers`** is now 32 (was 24) to fit cspot's two
  `/spotify_info` handlers.
- **SDKCONFIG cache**: changes to `sdkconfig.defaults*` only re-apply when you
  delete the generated `sdkconfig.esp32p4` first.

## Don't do this

- Don't `git rm` `components/cspot/cspot_lib/bell/external/` — even with
  `BELL_DISABLE_*` flags, several add_subdirectory calls reference those dirs.
- Don't try to use ESP-IDF's `protobuf-c` component instead of nanopb —
  cspot's `.proto` files target nanopb specifically.
- Don't enable `BELL_DISABLE_CODECS=OFF` here — the codec subdirs pull in opus,
  helix-mp3 etc. that we don't need and would conflict with `esp_audio_codec`.
- Don't add `PRIV_REQUIRES main` to `spotify_connect` — it would create a
  REQUIRES cycle. The shim uses forward declarations for audio_output_* /
  playback_control_set_source instead.

## Hardware

- **Board**: Guition ESP32-P4-M3-DEV (wiring matches Waveshare ESP32-P4-DEV-KIT).
  Same `iot_board.h` profile, GPIO54 active-HIGH reset for the C6.
- **I2S pins** for the external DAC: BCK=4, WS=22, DO=5 (configurable via
  Kconfig under "Audio output").
- **SDIO** to C6: CMD=19, CLK=18, D0-3=14-17 (4-bit, 20 MHz).
- **USB-UART**: CH340 enumerates as `/dev/cu.usbserial-2120` on the dev Mac.
