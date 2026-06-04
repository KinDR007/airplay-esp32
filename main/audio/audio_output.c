#include "audio_output.h"

#include "audio_receiver.h"
#include "audio_resample.h"
#include "led.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rtsp_server.h"
#include <inttypes.h>
#include <stdlib.h>

// SIDE NOTE; providing power from GPIO pins is capped ~20mA.
#if CONFIG_I2S_GND_IO >= 0
#define I2S_GND_PIN CONFIG_I2S_GND_IO
#endif
#if CONFIG_I2S_VCC_IO >= 0
#define I2S_VCC_PIN CONFIG_I2S_VCC_IO
#endif
#if CONFIG_DAC_SOFT_MUTE_GPIO >= 0
#define DAC_MUTE_PIN CONFIG_DAC_SOFT_MUTE_GPIO
#endif

#define TAG           "audio_output"
#define I2S_SCK_PIN   CONFIG_I2S_SCK_IO
#define I2S_BCK_PIN   CONFIG_I2S_BCK_IO
#define I2S_LRCK_PIN  CONFIG_I2S_WS_IO
#define I2S_DOUT_PIN  CONFIG_I2S_DO_IO
#define OUTPUT_RATE   CONFIG_OUTPUT_SAMPLE_RATE_HZ
#define FRAME_SAMPLES 352

/* Max output frames after resampling one input frame */
#define MAX_RESAMPLE_FRAMES \
  ((size_t)((FRAME_SAMPLES + 2) * ((double)OUTPUT_RATE / 44100) + 16))

#if CONFIG_FREERTOS_UNICORE
#define PLAYBACK_CORE 0
#else
#define PLAYBACK_CORE 1
#endif

static i2s_chan_handle_t tx_handle;
static volatile bool flush_requested = false;
static volatile bool playback_running = false;
static TaskHandle_t playback_task_handle = NULL;
static volatile int source_rate = 44100;
static volatile bool resample_reinit_needed = false;

// ---- Smooth connect/disconnect fade --------------------------------------
// A Q15 gain (0..32768) multiplied on top of the normal volume. Output starts
// muted and fades in when audio begins after a (re)connect, and is ramped back
// down to zero before audio is cut on disconnect — so transitions aren't an
// abrupt jump. The gain steps once per stereo frame inside apply_volume().
#define FADE_IN_MS  500
#define FADE_OUT_MS 180
// After this many consecutive silent reads (no client / paused / stopped /
// underrun) the playback task soft-mutes the DAC so an idle line-out doesn't
// hiss through the amp. ~0.4 s at the output frame rate — long enough not to
// trip on brief mid-stream gaps, short enough to kill idle hiss quickly.
#define IDLE_MUTE_FRAMES ((2 * OUTPUT_RATE) / (5 * FRAME_SAMPLES))
static volatile int32_t s_fade_gain   = 0;     // current gain, stepped per frame
static volatile int32_t s_fade_target = 0;     // 0 = silent, 32768 = unity
static volatile int32_t s_fade_step   = 0;     // Q15 delta per stereo frame
static volatile bool s_fade_in_pending = true; // fade in when audio next resumes
static volatile bool s_user_muted = false;     // explicit mute (e.g. from HA)

// Drive the DAC's soft-mute pin (e.g. PCM5102A XSMT): LOW = mute, HIGH = play.
// The DAC ramps internally, so toggling it is click-free. No-op if unconfigured.
static void dac_set_mute(bool mute) {
#ifdef DAC_MUTE_PIN
  gpio_set_level(DAC_MUTE_PIN, mute ? 0 : 1);
#else
  (void)mute;
#endif
}

static void fade_to(int32_t target, uint32_t ms) {
  uint32_t frames = (ms * (uint32_t)OUTPUT_RATE) / 1000u;
  if (frames == 0) frames = 1;
  int32_t delta = target - s_fade_gain;
  int32_t step = delta / (int32_t)frames;
  if (step == 0) step = (delta >= 0) ? 1 : -1;
  s_fade_step = step;
  s_fade_target = target;
}

void audio_output_fade_in(uint32_t ms) {
  dac_set_mute(false); // un-mute the DAC before ramping the digital gain up
  fade_to(32768, ms ? ms : 1);
}
void audio_output_fade_out(uint32_t ms) { fade_to(0, ms ? ms : 1); }

void audio_output_fade_out_wait(uint32_t ms) {
  audio_output_fade_out(ms);
  // Block until the playback task has rendered the ramp down (or give up, e.g.
  // if the buffer ran dry / playback isn't running). Caller then stops audio.
  uint32_t waited = 0, budget = ms + 80;
  while (s_fade_gain > 0 && waited < budget) {
    vTaskDelay(pdMS_TO_TICKS(5));
    waited += 5;
  }
  dac_set_mute(true); // analog-mute the DAC now that it's faded to silence
  // Now that we've faded to silence, arm a fade-in so the next time audio
  // resumes it ramps up (and un-mutes) smoothly from zero.
  s_fade_in_pending = true;
}

// Explicit user mute (e.g. the Home Assistant mute button). Independent of the
// source/AirPlay volume: ramps the local output to silence (+ DAC soft-mute)
// and holds it there — surviving track changes/reconnects — until unmuted.
void audio_output_set_mute(bool mute) {
  if (mute == s_user_muted) return;
  s_user_muted = mute;
  if (mute) {
    audio_output_fade_out(FADE_OUT_MS); // ramp to silence (click-free)
    dac_set_mute(true);
  } else {
    dac_set_mute(false);
    audio_output_fade_in(FADE_IN_MS); // ramp back up
  }
}

bool audio_output_is_muted(void) { return s_user_muted; }

static void apply_volume(int16_t *buf, size_t n) {
#ifdef CONFIG_DAC_CONTROLS_VOLUME
  int32_t vol = 32768; // DAC handles volume in hardware; fade still applies
#else
  int32_t vol = airplay_get_volume_q15();
#endif
#if CONFIG_OUTPUT_GAIN_PERCENT < 100
  // Master output attenuation (line-out too hot for the amp). Scales the whole
  // volume range down so the full AirPlay slider stays usable.
  vol = (vol * CONFIG_OUTPUT_GAIN_PERCENT) / 100;
#endif
  int32_t g = s_fade_gain;
  const int32_t target = s_fade_target;
  const int32_t step = s_fade_step;
  // Fast path: unity volume, fully faded in — nothing to scale.
  if (vol >= 32768 && g >= 32768 && target >= 32768) return;
  for (size_t i = 0; i + 1 < n; i += 2) {
    if (g != target) {
      g += step;
      if ((step > 0 && g > target) || (step < 0 && g < target)) g = target;
    }
    int32_t eff = (vol * g) >> 15; // combined Q15 gain
    buf[i] = (int16_t)(((int32_t)buf[i] * eff) >> 15);
    buf[i + 1] = (int16_t)(((int32_t)buf[i + 1] * eff) >> 15);
  }
  s_fade_gain = g;
}

static void playback_task(void *arg) {
  int16_t *pcm = malloc((size_t)(FRAME_SAMPLES + 1) * 2 * sizeof(int16_t));
  int16_t *silence = calloc((size_t)FRAME_SAMPLES * 2, sizeof(int16_t));
  int16_t *resample_buf = malloc(MAX_RESAMPLE_FRAMES * 2 * sizeof(int16_t));
  if (!pcm || !silence || !resample_buf) {
    ESP_LOGE(TAG, "Failed to allocate buffers");
    free(pcm);
    free(silence);
    playback_task_handle = NULL;
    free(resample_buf);
    vTaskDelete(NULL);
    return;
  }

  size_t written;
  uint32_t silent_run = 0; // consecutive reads with no audio (idle detection)
  while (playback_running) {
    if (resample_reinit_needed) {
      resample_reinit_needed = false;
      audio_resample_init((uint32_t)source_rate, OUTPUT_RATE, 2);
    }
    if (flush_requested) {
      flush_requested = false;
      audio_resample_reset();
      // NOTE: deliberately do NOT disable/enable the I2S channel here. Cycling
      // it stops BCK, and the PCM5102A (BCK-only mode, SCK=GND) re-locks its
      // internal PLL with an audible pop. Keeping the channel running — the
      // task writes silence between streams — keeps the DAC clocked and quiet.
      // Also do NOT touch the fade gain here: FLUSH fires on every resume /
      // seek / track change (realtime mode: TEARDOWN→SETUP→FLUSH→RECORD), so
      // dropping gain to 0 would dip audio that's already playing. The gain is
      // only driven to 0 on a genuine pause/disconnect (audio_output_fade_out),
      // which also arms the fade-in for the next start.
    }
    size_t samples = audio_receiver_read(pcm, FRAME_SAMPLES + 1);
    if (samples > 0) {
      silent_run = 0;
      if (s_fade_in_pending && !s_user_muted) {
        s_fade_in_pending = false;
        audio_output_fade_in(FADE_IN_MS);
      }
      int16_t *play_buf = pcm;
      size_t play_samples = samples;
      if (audio_resample_is_active()) {
        play_samples = audio_resample_process(pcm, samples, resample_buf,
                                              MAX_RESAMPLE_FRAMES);
        play_buf = resample_buf;
      }
      apply_volume(play_buf, play_samples * 2);
      led_audio_feed(play_buf, play_samples);
      i2s_channel_write(tx_handle, play_buf, play_samples * 4, &written,
                        portMAX_DELAY);
      taskYIELD();
    } else {
      // No audio. After a short spell of continuous silence the source is idle
      // (no client / paused / stopped), so soft-mute the DAC to kill line-out
      // hiss and arm a fade-in so the next audio ramps up cleanly.
      if (silent_run < IDLE_MUTE_FRAMES) {
        if (++silent_run == IDLE_MUTE_FRAMES) {
          s_fade_gain = 0;
          s_fade_target = 0;
          s_fade_in_pending = true;
          dac_set_mute(true);
        }
      }
      led_audio_feed(silence, FRAME_SAMPLES);
      i2s_channel_write(tx_handle, silence, (size_t)FRAME_SAMPLES * 4, &written,
                        pdMS_TO_TICKS(10));
      vTaskDelay(1);
    }
  }

  free(pcm);
  free(silence);
  playback_task_handle = NULL;
  vTaskDelete(NULL);
}

esp_err_t audio_output_init(void) {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.dma_desc_num = 8;
  chan_cfg.dma_frame_num = 256;

  ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &tx_handle, NULL), TAG,
                      "channel create failed");

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(OUTPUT_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = I2S_SCK_PIN,
              .bclk = I2S_BCK_PIN,
              .ws = I2S_LRCK_PIN,
              .dout = I2S_DOUT_PIN,
              .din = I2S_GPIO_UNUSED,
          },
  };
#ifdef I2S_GND_PIN
  gpio_reset_pin(I2S_GND_PIN);
  gpio_set_direction(I2S_GND_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(I2S_GND_PIN, 0);
#endif
#ifdef I2S_VCC_PIN
  gpio_reset_pin(I2S_VCC_PIN);
  gpio_set_direction(I2S_VCC_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(I2S_VCC_PIN, 1);
#endif
#ifdef DAC_MUTE_PIN
  // Start muted; the playback task unmutes via fade-in when audio begins.
  gpio_reset_pin(DAC_MUTE_PIN);
  gpio_set_direction(DAC_MUTE_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(DAC_MUTE_PIN, 0);
#endif

  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx_handle, &std_cfg), TAG,
                      "std mode init failed");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(tx_handle), TAG,
                      "channel enable failed");

  audio_resample_init(44100, OUTPUT_RATE, 2);

  return ESP_OK;
}

void audio_output_start(void) {
  if (playback_task_handle != NULL) {
    return; // already running
  }
  playback_running = true;
  xTaskCreatePinnedToCore(playback_task, "audio_play", 4096, NULL, 7,
                          &playback_task_handle, PLAYBACK_CORE);
}

void audio_output_stop(void) {
  if (playback_task_handle == NULL) {
    return;
  }
  playback_running = false;
  // Wait for task to exit cleanly
  int timeout = 40;
  while (playback_task_handle != NULL && timeout-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (playback_task_handle != NULL) {
    ESP_LOGW(TAG, "Playback task did not exit within timeout");
  } else {
    ESP_LOGI(TAG, "Playback task stopped");
  }
}

esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait) {
  size_t written = 0;
  return i2s_channel_write(tx_handle, data, bytes, &written, wait);
}

void audio_output_set_sample_rate(uint32_t rate) {
  // Only safe to call when no writer task is actively using I2S
  // (AirPlay playback task must be stopped, BT calls this before
  // the I2S writer task starts consuming data)
  ESP_LOGI(TAG, "Setting sample rate to %" PRIu32 " Hz", rate);
  i2s_channel_disable(tx_handle);
  i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
  i2s_channel_reconfig_std_clock(tx_handle, &clk_cfg);
  i2s_channel_enable(tx_handle);
}

void audio_output_flush(void) {
  flush_requested = true;
}

void audio_output_set_source_rate(int rate) {
  if (rate > 0 && rate != source_rate) {
    source_rate = rate;
    resample_reinit_needed = true;
  }
}
