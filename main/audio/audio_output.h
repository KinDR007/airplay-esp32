#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "freertos/FreeRTOS.h"

/**
 * Initialize the audio output backend (I2S / SPDIF / USB UAC).
 */
esp_err_t audio_output_init(void);

/**
 * Start the audio playback task.
 */
void audio_output_start(void);

/**
 * Flush output buffers (clears stale audio on pause/seek).
 */
void audio_output_flush(void);

/**
 * Stop the AirPlay playback task (for yielding I2S to another source)
 */
void audio_output_stop(void);

/**
 * Write raw PCM data to the I2S output.
 * Can be used by any audio source (BT A2DP, etc.) when the AirPlay
 * playback task is stopped.
 *
 * @param data   PCM data buffer (interleaved stereo, 16-bit)
 * @param bytes  Number of bytes to write
 * @param wait   Maximum ticks to wait for I2S DMA space
 * @return ESP_OK on success
 */
esp_err_t audio_output_write(const void *data, size_t bytes, TickType_t wait);

/**
 * Change the I2S sample rate (e.g. when BT negotiates 48 kHz)
 *
 * @param rate  Sample rate in Hz (e.g. 44100, 48000)
 */
void audio_output_set_sample_rate(uint32_t rate);

/**
 * Notify the output of the source sample rate (from AirPlay ANNOUNCE).
 * The resampler is re-initialized if the rate changes.
 */
void audio_output_set_source_rate(int rate);

/**
 * Smoothly ramp the output gain up to full over @p ms milliseconds.
 * Normally triggered automatically when audio resumes after a flush.
 */
void audio_output_fade_in(uint32_t ms);

/**
 * Smoothly ramp the output gain down to zero over @p ms milliseconds
 * (non-blocking).
 */
void audio_output_fade_out(uint32_t ms);

/**
 * Ramp the output gain down to zero and block until the fade has been
 * rendered (or a short timeout elapses). Call this right before stopping
 * audio on disconnect so the cut isn't abrupt.
 */
void audio_output_fade_out_wait(uint32_t ms);

/**
 * Explicit user mute of the local output (independent of source volume).
 * Ramps to silence + DAC soft-mute and holds until unmuted. Used by the
 * Home Assistant mute button via POST /api/control?cmd=mute.
 */
void audio_output_set_mute(bool mute);

/** Current explicit-mute state (for status reporting). */
bool audio_output_is_muted(void);
