#ifndef AUDIO_RECORDER_H
#define AUDIO_RECORDER_H

#include <Arduino.h>
#include "driver/i2s.h"
#include "esp_heap_caps.h"
#include "wav_generator.h"

#define SAMPLE_RATE 16000U
#define SAMPLE_BITS 16
// Digital boost applied while recording. The INMP441 already delivers high levels (raw
// peaks reach ~26000 on loud speech), so any fixed shift saturates: <<3 clipped
// everything above |4096|, <<2 still clips above |16383|, turning speech into a square
// wave before it ever reaches the model. Leveling is applied once after recording
// instead (log_and_normalize_recorded_audio), which never introduces clipping.
#define VOLUME_GAIN 0
#define AUDIO_NORMALIZE_TARGET_PEAK 26200   // ~80% of int16 full scale
#define AUDIO_NORMALIZE_MAX_GAIN 8.0f       // never amplify more than this
#define AUDIO_NORMALIZE_MIN_PEAK 300        // below this the clip counts as silence
#define HPF_ALPHA 0.97f
#define MAX_AUDIO_SIZE 500000
#define AUDIO_READ_CHUNK_SIZE 512
#define MAX_RECORD_TIME_SECONDS 10
#define MIC_TRIGGER_P2P_THRESHOLD 7000

#define I2S_NUM I2S_NUM_0
#define I2S_SCK GPIO_NUM_13
#define I2S_WS GPIO_NUM_12
#define I2S_DIN GPIO_NUM_11
#define BUTTON_PIN 1

extern i2s_port_t i2s_port;
extern bool recording_active;
extern uint8_t* audio_buffer;
extern size_t audio_buffer_size;
const uint32_t rtime = 10;

bool init_i2s_inmp441() {
  Serial.println("Initializing I2S for INMP441...");
  
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = 0,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  esp_err_t err = i2s_driver_install(i2s_port, &i2s_config, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("Failed to install I2S driver: %d\n", err);
    return false;
  }

  i2s_pin_config_t pin_config = {0};
  pin_config.bck_io_num = I2S_SCK;
  pin_config.ws_io_num = I2S_WS;
  pin_config.data_out_num = I2S_PIN_NO_CHANGE;
  pin_config.data_in_num = I2S_DIN;
  pin_config.mck_io_num = I2S_PIN_NO_CHANGE;

  err = i2s_set_pin(i2s_port, &pin_config);
  if (err != ESP_OK) {
    Serial.printf("Failed to set I2S pins: %d\n", err);
    i2s_driver_uninstall(i2s_port);
    return false;
  }

  Serial.println("I2S for INMP441 initialized successfully");
  return true;
}

void deinit_i2s() {
  if (i2s_port >= 0) {
    i2s_driver_uninstall(i2s_port);
    Serial.println("I2S deinitialized");
  }
}

bool detect_microphone_activity(uint16_t p2pThreshold = MIC_TRIGGER_P2P_THRESHOLD, uint32_t listenMs = 120) {
  uint8_t read_buffer[AUDIO_READ_CHUNK_SIZE];
  unsigned long start = millis();
  int16_t maxSample = INT16_MIN;
  int16_t minSample = INT16_MAX;
  bool gotData = false;

  while (millis() - start < listenMs) {
    size_t bytes_read = 0;
    esp_err_t result = i2s_read(i2s_port, read_buffer, sizeof(read_buffer), &bytes_read, pdMS_TO_TICKS(20));
    if (result != ESP_OK || bytes_read < 2) {
      continue;
    }

    gotData = true;
    for (size_t i = 0; i + 1 < bytes_read; i += 2) {
      int16_t sample = (int16_t)((read_buffer[i + 1] << 8) | read_buffer[i]);
      if (sample > maxSample) maxSample = sample;
      if (sample < minSample) minSample = sample;
    }
  }

  if (!gotData) {
    Serial.println(">>> Mic activity check: no data read from INMP441");
    return false;
  }

  int32_t p2p = (int32_t)maxSample - (int32_t)minSample;
  Serial.printf(">>> Mic peak-to-peak: %ld (min=%d, max=%d, threshold=%u)\r\n",
                (long)p2p, minSample, maxSample, p2pThreshold);
  return p2p >= p2pThreshold;
}


// Reports how loud the recorded clip actually is, then scales it so the loudest sample
// sits near full scale. This replaces the old fixed digital boost, so loud shouting is
// never clipped into a square wave, while quiet recordings still get lifted.
void log_and_normalize_recorded_audio(size_t data_offset, size_t total_bytes) {
  if (!audio_buffer || total_bytes <= data_offset + 1) {
    return;
  }

  int16_t* samples = (int16_t*)&audio_buffer[data_offset];
  const size_t sample_count = (total_bytes - data_offset) / 2;
  int32_t peak = 0;
  size_t full_scale_samples = 0;

  Serial.print(">>> Per-second RMS:");
  double sum_squares = 0.0;
  size_t samples_in_second = 0;

  for (size_t i = 0; i < sample_count; i++) {
    int32_t sample = samples[i];
    int32_t magnitude = sample < 0 ? -sample : sample;
    if (magnitude > peak) {
      peak = magnitude;
    }
    if (magnitude >= 32767) {
      full_scale_samples++;
    }
    sum_squares += (double)sample * (double)sample;
    samples_in_second++;

    if (samples_in_second == SAMPLE_RATE || i + 1 == sample_count) {
      Serial.printf(" %d", (int)sqrt(sum_squares / samples_in_second));
      sum_squares = 0.0;
      samples_in_second = 0;
    }
  }
  Serial.println();

  Serial.printf(">>> Clip peak=%ld, samples at full scale=%u (%.2f%%)\n",
                (long)peak, (unsigned)full_scale_samples,
                100.0f * (float)full_scale_samples / (float)sample_count);

  if (peak < AUDIO_NORMALIZE_MIN_PEAK) {
    Serial.println(">>> Recording is near silent - skipping normalization");
    return;
  }
  if (peak >= AUDIO_NORMALIZE_TARGET_PEAK) {
    Serial.println(">>> Level already optimal - skipping normalization");
    return;
  }

  float gain = (float)AUDIO_NORMALIZE_TARGET_PEAK / (float)peak;
  if (gain > AUDIO_NORMALIZE_MAX_GAIN) {
    gain = AUDIO_NORMALIZE_MAX_GAIN;
  }

  for (size_t i = 0; i < sample_count; i++) {
    int32_t scaled = (int32_t)((float)samples[i] * gain);
    if (scaled > 32767) scaled = 32767;
    if (scaled < -32768) scaled = -32768;
    samples[i] = (int16_t)scaled;
  }

  Serial.printf(">>> Normalized quiet recording: gain x%.2f (peak %ld -> %d)\n",
                gain, (long)peak, (int)((float)peak * gain));
}

void record_wav_to_ram() {
  Serial.printf("Free heap before allocation: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
  Serial.printf("Requesting: %d bytes\n\n", MAX_AUDIO_SIZE);
  
  audio_buffer = (uint8_t*)heap_caps_malloc(MAX_AUDIO_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!audio_buffer) {
    Serial.println("WARNING: PSRAM allocation failed, falling back to internal heap");
    audio_buffer = (uint8_t*)malloc(MAX_AUDIO_SIZE);
  }
  if (!audio_buffer) {
    Serial.println("ERROR: Failed to allocate audio buffer!");
    Serial.printf("Free heap after failed allocation: %d bytes\n", ESP.getFreeHeap());
    return;
  }

  uint8_t wav_header[WAV_HEADER_SIZE];
  generate_wav_header(wav_header, 0, SAMPLE_RATE, SAMPLE_BITS);
  memcpy(audio_buffer, wav_header, WAV_HEADER_SIZE);

  uint8_t* read_buffer = (uint8_t*)malloc(512);
  if (!read_buffer) {
    Serial.println("ERROR: Failed to allocate read buffer!");
    free(audio_buffer);
    audio_buffer = nullptr;
    return;
  }

  recording_active = true;
  size_t total_bytes = WAV_HEADER_SIZE;
  unsigned long startTime = millis();
  const uint32_t max_record_time = rtime;
  float hpf_prev_input = 0.0f;
  float hpf_prev_output = 0.0f;

  Serial.println("Recording audio to RAM...");
  Serial.printf("Max buffer: %d bytes (~%.1f seconds)\n", 
                MAX_AUDIO_SIZE, (float)(MAX_AUDIO_SIZE - WAV_HEADER_SIZE) / (SAMPLE_RATE * 2));
  Serial.printf("Recording for %d seconds...\n", max_record_time);

  while ((millis() - startTime < max_record_time * 1000)) {

    size_t bytes_read = 0;
    esp_err_t result = i2s_read(i2s_port, read_buffer, 512, &bytes_read, pdMS_TO_TICKS(100));
    
    if (result != ESP_OK) {
      continue;
    }

    if (total_bytes + bytes_read > MAX_AUDIO_SIZE) {
      Serial.println("\n>>> Audio buffer full - stopping recording");
      break;
    }

    for (size_t i = 0; i < bytes_read; i += 2) {
      int16_t* sample = (int16_t*)&read_buffer[i];
      float input = (float)(*sample);
      float filtered = HPF_ALPHA * (hpf_prev_output + input - hpf_prev_input);

      hpf_prev_input = input;
      hpf_prev_output = filtered;

      int32_t amp = ((int32_t)filtered) << VOLUME_GAIN;
      
      if (amp > 32767) amp = 32767;
      if (amp < -32768) amp = -32768;
      
      *sample = (int16_t)amp;
    }

    memcpy(audio_buffer + total_bytes, read_buffer, bytes_read);
    total_bytes += bytes_read;
  }

  recording_active = false;
  free(read_buffer);

  size_t audio_data_size = total_bytes - WAV_HEADER_SIZE;
  generate_wav_header(wav_header, audio_data_size, SAMPLE_RATE, SAMPLE_BITS);
  memcpy(audio_buffer, wav_header, WAV_HEADER_SIZE);

  audio_buffer_size = total_bytes;

  log_and_normalize_recorded_audio(WAV_HEADER_SIZE, audio_buffer_size);

  int16_t max_amplitude = 0;
  int16_t min_amplitude = 0;
  for (size_t i = WAV_HEADER_SIZE; i < total_bytes; i += 2) {
    int16_t* sample = (int16_t*)&audio_buffer[i];
    if (*sample > max_amplitude) max_amplitude = *sample;
    if (*sample < min_amplitude) min_amplitude = *sample;
  }

  float duration = (float)audio_data_size / (SAMPLE_RATE * 2);
  Serial.printf("\n>>> Recording finished!");
  Serial.printf(" Total: %d bytes (%.2f seconds)\n", total_bytes, duration);
  Serial.printf(">>> Audio Analysis: Max=%d, Min=%d, Peak-to-Peak=%d\n\n", 
                max_amplitude, min_amplitude, max_amplitude - min_amplitude);
}


void record_audio_from_microphone() {
  Serial.println("\n>>> STEP 1: Recording audio from microphone...");
  record_wav_to_ram();
  
  if (audio_buffer != nullptr && audio_buffer_size > 0) {
    Serial.println(">>> Recording successful!");
  } else {
    Serial.println(">>> ERROR: Recording failed!");
  }
}

// Releases the recording buffer so the next capture starts from a clean PSRAM state.
void cleanup_audio_buffer() {
  if (audio_buffer != nullptr) {
    free(audio_buffer);
    audio_buffer = nullptr;
    audio_buffer_size = 0;
    Serial.println(">>> Audio buffer freed - RAM available for next recording\n");
  }
}

void free_audio_memory() {
  Serial.println("\n>>> STEP 3: Cleaning up audio buffer...");
  cleanup_audio_buffer();
}

#endif
