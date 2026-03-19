#ifndef OPENAI_WHISPER_H
#define OPENAI_WHISPER_H

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "chatgpt_handler.h"
#include "audio_recorder.h"

// OpenAI Whisper API Configuration
extern const char* openai_api_key;
extern const char* openai_whisper_url;
extern const char* whisper_model;

extern bool wifi_connected;
extern uint8_t* audio_buffer;
extern size_t audio_buffer_size;
extern String last_transcription;
extern String transcription;

bool parse_whisper_response(const String& response);

// Validate audio and WiFi status
bool validate_audio_and_wifi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: WiFi not connected");
    return false;
  }
  
  if (!audio_buffer || audio_buffer_size == 0) {
    Serial.println("ERROR: No audio data available");
    return false;
  }

  if (audio_buffer_size > 26214400) {
    Serial.println("ERROR: Audio file exceeds 25MB limit");
    return false;
  }

  return true;
}

// Build multipart form data
size_t build_multipart_request(size_t payload_size, String& boundary, String& body_start, String& body_end) {
  boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
  
  body_start = "--" + boundary + "\r\n";
  body_start += "Content-Disposition: form-data; name=\"model\"\r\n\r\n";
  body_start += whisper_model;
  body_start += "\r\n";
  body_start += "--" + boundary + "\r\n";
  body_start += "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n";
  body_start += "json";
  body_start += "\r\n";
  body_start += "--" + boundary + "\r\n";
  body_start += "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
  body_start += "Content-Type: audio/wav\r\n\r\n";

  body_end = "\r\n--" + boundary + "--\r\n";

  size_t start_len = body_start.length();
  size_t end_len = body_end.length();
  size_t total_size = start_len + payload_size + end_len;

  return total_size;
}

// Prepare request buffer
bool prepare_request_buffer(const String& body_start, const String& body_end, size_t total_size) {
  Serial.printf("Reusing audio buffer for complete request: %d bytes\n", total_size);
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

  if (total_size > MAX_AUDIO_SIZE) {
    Serial.println("ERROR: Complete request exceeds buffer size!");
    return false;
  }

  size_t start_len = body_start.length();
  size_t end_len = body_end.length();

  memmove(audio_buffer + start_len, audio_buffer, audio_buffer_size);
  memcpy(audio_buffer, body_start.c_str(), start_len);
  memcpy(audio_buffer + start_len + audio_buffer_size, body_end.c_str(), end_len);

  return true;
}

// Parse Whisper API response
bool parse_whisper_response(const String& response) {
  Serial.println(">>> HTTP 200 OK - Parsing response...");
  
  JsonDocument doc;
  if (deserializeJson(doc, response) == DeserializationError::Ok) {
    if (doc["text"].is<String>()) {
      transcription = doc["text"].as<String>();
      last_transcription = transcription;
      
      Serial.println("\n========================================");
      Serial.println("TRANSCRIPTION RESULT (OpenAI Whisper):");
      Serial.println("========================================");
      Serial.println(transcription);
      Serial.println("========================================\n");
      return true;
    } else {
      Serial.println("ERROR: 'text' field not found in response");
      return false;
    }
  } else {
    Serial.println("ERROR: Failed to parse JSON response");
    return false;
  }
}

// Send to OpenAI Whisper API
void send_to_openai_whisper() {
  if (!validate_audio_and_wifi()) {
    return;
  }

  Serial.printf("Free heap before Whisper HTTPS: %u bytes\n", ESP.getFreeHeap());
  if (ESP.getFreeHeap() < 45000) {
    Serial.println("ERROR: Not enough free heap for TLS connection. Try shorter recording.");
    return;
  }

  WiFiClientSecure secure_client;
  secure_client.setInsecure();

  HTTPClient http;
  if (!http.begin(secure_client, openai_whisper_url)) {
    Serial.println("ERROR: Failed to initialize HTTP connection");
    return;
  }

  http.setTimeout(30000);
  http.setConnectTimeout(10000);
  
  String auth_header = "Bearer ";
  auth_header += openai_api_key;
  http.addHeader("Authorization", auth_header);

  String boundary, body_start, body_end;
  size_t total_size = build_multipart_request(audio_buffer_size, boundary, body_start, body_end);

  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  if (!prepare_request_buffer(body_start, body_end, total_size)) {
    http.end();
    return;
  }

  Serial.println("Sending HTTP POST request...");

  int httpResponseCode = http.POST(audio_buffer, total_size);
  String response = http.getString();

  if (httpResponseCode == 200) {
    parse_whisper_response(response);
  } else {
    Serial.printf(">>> HTTP Error: %d\n", httpResponseCode);
    Serial.printf("Response: %s\n", response.c_str());
  }

  http.end();
}

// Clean up audio buffer
void cleanup_audio_buffer() {
  if (audio_buffer != nullptr) {
    free(audio_buffer);
    audio_buffer = nullptr;
    audio_buffer_size = 0;
    Serial.println(">>> Audio buffer freed - RAM available for next recording\n");
  }
}

// MAIN WORKFLOW FUNCTIONS (Called from main.cpp)

// Step 2: Convert recorded audio to text using Whisper API
String convert_audio_to_text() {
  Serial.println("\n>>> STEP 2: Converting audio to text with Whisper API...");
  
  transcription = "";  // Reset transcription
  send_to_openai_whisper();
  
  if (transcription.length() > 0) {
    Serial.println(">>> Transcription successful!");
    return transcription;
  } else {
    Serial.println(">>> ERROR: Transcription failed!");
    return "";
  }
}

// Step 4: Free audio memory
void free_audio_memory() {
  Serial.println("\n>>> STEP 5: Cleaning up audio buffer...");
  cleanup_audio_buffer();
}

#endif
