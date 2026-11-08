#ifndef WAV_GENERATOR_H
#define WAV_GENERATOR_H

#include <Arduino.h>

#define WAV_HEADER_SIZE 44

void generate_wav_header(uint8_t* wav_header, uint32_t wav_size, uint32_t sample_rate, uint16_t sample_bits) {
  uint32_t file_size = wav_size + WAV_HEADER_SIZE - 8;
  uint32_t byte_rate = sample_rate * sample_bits / 8;

  const uint8_t header[] = {
    'R', 'I', 'F', 'F',
    file_size, file_size >> 8, file_size >> 16, file_size >> 24,
    
    'W', 'A', 'V', 'E',
    
    'f', 'm', 't', ' ',
    0x10, 0x00, 0x00, 0x00,
    0x01, 0x00,
    0x01, 0x00,
    
    sample_rate, sample_rate >> 8, sample_rate >> 16, sample_rate >> 24,
    
    byte_rate, byte_rate >> 8, byte_rate >> 16, byte_rate >> 24,
    
    0x02, 0x00,
    0x10, 0x00,
    
    'd', 'a', 't', 'a',
    wav_size, wav_size >> 8, wav_size >> 16, wav_size >> 24
  };

  memcpy(wav_header, header, sizeof(header));
}

#endif
