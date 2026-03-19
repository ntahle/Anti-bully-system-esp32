#ifndef WAV_GENERATOR_H
#define WAV_GENERATOR_H

#include <Arduino.h>

#define WAV_HEADER_SIZE 44

// Generate WAV file header
void generate_wav_header(uint8_t* wav_header, uint32_t wav_size, uint32_t sample_rate, uint16_t sample_bits) {
  uint32_t file_size = wav_size + WAV_HEADER_SIZE - 8;
  uint32_t byte_rate = sample_rate * sample_bits / 8;

  const uint8_t header[] = {
    // RIFF chunk
    'R', 'I', 'F', 'F',
    file_size, file_size >> 8, file_size >> 16, file_size >> 24,
    
    // WAVE format
    'W', 'A', 'V', 'E',
    
    // fmt subchunk
    'f', 'm', 't', ' ',
    0x10, 0x00, 0x00, 0x00,  // Subchunk1Size (16 for PCM)
    0x01, 0x00,              // AudioFormat (1 for PCM)
    0x01, 0x00,              // NumChannels (1 for mono)
    
    // Sample rate (little-endian)
    sample_rate, sample_rate >> 8, sample_rate >> 16, sample_rate >> 24,
    
    // Byte rate (little-endian)
    byte_rate, byte_rate >> 8, byte_rate >> 16, byte_rate >> 24,
    
    // Block align and bits per sample
    0x02, 0x00,              // BlockAlign (2 bytes for 16-bit mono)
    0x10, 0x00,              // BitsPerSample (16)
    
    // Data subchunk
    'd', 'a', 't', 'a',
    wav_size, wav_size >> 8, wav_size >> 16, wav_size >> 24
  };

  memcpy(wav_header, header, sizeof(header));
}

#endif
