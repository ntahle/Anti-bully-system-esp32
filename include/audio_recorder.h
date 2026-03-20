#ifndef AUDIO_RECORDER_H
#define AUDIO_RECORDER_H

#include <Arduino.h>
#include "driver/i2s.h"
#include "wav_generator.h"

// Audio Configuration
#define SAMPLE_RATE 16000U
#define SAMPLE_BITS 16
#define VOLUME_GAIN 3
#define HPF_ALPHA 0.97f
#define MAX_AUDIO_SIZE 110000
#define AUDIO_READ_CHUNK_SIZE 512
#define MAX_RECORD_TIME_SECONDS 30

// I2S Configuration for INMP441
#define I2S_NUM I2S_NUM_0
#define I2S_SCK GPIO_NUM_26
#define I2S_WS GPIO_NUM_33
#define I2S_DIN GPIO_NUM_27
#define BUTTON_PIN 15


// Global variables
extern i2s_port_t i2s_port;
extern bool recording_active;
extern uint8_t* audio_buffer;
extern size_t audio_buffer_size;

// I2S Initialization for INMP441
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


// Record WAV to RAM
void record_wav_to_ram() {
  Serial.printf("Free heap before allocation: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());
  Serial.printf("Requesting: %d bytes\n\n", MAX_AUDIO_SIZE);
  
  audio_buffer = (uint8_t*)malloc(MAX_AUDIO_SIZE);
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
  const uint32_t max_record_time = 3;  // Fixed 3-second recording
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

    // Apply high-pass filter (remove low-frequency rumble/DC) then gain
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

  // Analyze audio amplitude
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

// MAIN WORKFLOW FUNCTION (Called from main.cpp)

// Step 1: Record audio from microphone
void record_audio_from_microphone() {
  Serial.println("\n>>> STEP 1: Recording audio from microphone...");
  record_wav_to_ram();
  
  if (audio_buffer != nullptr && audio_buffer_size > 0) {
    Serial.println(">>> Recording successful!");
  } else {
    Serial.println(">>> ERROR: Recording failed!");
  }
}

#endif
