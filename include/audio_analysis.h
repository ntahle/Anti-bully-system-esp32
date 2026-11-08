#ifndef AUDIO_ANALYSIS_H
#define AUDIO_ANALYSIS_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <mbedtls/base64.h>
#include "esp_heap_caps.h"

// One-shot audio analysis (WAV in -> analysis JSON out).
// Replaces the two-step flow (convert_audio_to_text + analyze_text_with_chatgpt)
// by sending the recorded WAV straight to an audio-input chat model in a single
// /v1/chat/completions request.
// Set SINGLE_PIPELINE_AUDIO_MODEL=0 in platformio.ini to fall back to two steps.
//
// Validated against the live API with a 16 kHz / 16-bit / mono WAV (same format as
// the ESP32 recordings), same system prompt as chatgpt_handler.h:
//   request : model=gpt-audio-1.5, modalities=["text"], max_tokens=400, temperature=0.2
//   response: choices[0].message.content is a plain JSON string, finish_reason=stop
//             {"label":"yes","confidence":95,"reasons":[...],"transcript":"..."}
//   usage   : 5.4 s of audio = 53 audio tokens (~10 tokens per second of audio),
//             285 text tokens, 56 completion tokens, ~5 s round trip

#ifndef SINGLE_PIPELINE_AUDIO_MODEL
#define SINGLE_PIPELINE_AUDIO_MODEL 1
#endif

extern const char* openai_api_key;
extern const char* openai_gpt_url;
extern String chatgpt_system_prompt;
extern String audio_model;

extern uint8_t* audio_buffer;
extern size_t audio_buffer_size;

#define AUDIO_MODEL_TIMEOUT_MS 60000              // HTTPClient::setTimeout is uint16_t
#define AUDIO_MODEL_CONNECT_TIMEOUT_MS 10000
#define AUDIO_MODEL_MAX_TOKENS 400                // JSON + transcript, 100 is too tight
#define AUDIO_MODEL_PSRAM_MARGIN 131072UL         // slack around the request body allocation
#define AUDIO_MODEL_MIN_FREE_HEAP 45000
#define AUDIO_MODEL_B64_PLACEHOLDER "AUDIO_BASE64_DATA_PLACEHOLDER"
#define AUDIO_MODEL_USER_INSTRUCTION "Analyze this recording for bullying indicators. Transcribe exactly what was said, then classify it."

// Serializes the request envelope with a placeholder where the base64 audio goes,
// so ArduinoJson takes care of escaping the system prompt (newlines, quotes).
bool build_audio_model_envelope(String& preamble, String& epilogue) {
  JsonDocument envelope;
  envelope["model"] = audio_model;
  envelope["modalities"][0] = "text";
  envelope["max_tokens"] = AUDIO_MODEL_MAX_TOKENS;
  envelope["temperature"] = 0.2;

  JsonArray messages = envelope["messages"].to<JsonArray>();

  JsonObject system_message = messages.add<JsonObject>();
  system_message["role"] = "system";
  system_message["content"] = chatgpt_system_prompt;

  JsonObject user_message = messages.add<JsonObject>();
  user_message["role"] = "user";
  JsonArray content = user_message["content"].to<JsonArray>();

  JsonObject text_part = content.add<JsonObject>();
  text_part["type"] = "text";
  text_part["text"] = AUDIO_MODEL_USER_INSTRUCTION;

  JsonObject audio_part = content.add<JsonObject>();
  audio_part["type"] = "input_audio";
  audio_part["input_audio"]["data"] = AUDIO_MODEL_B64_PLACEHOLDER;
  audio_part["input_audio"]["format"] = "wav";

  String payload;
  serializeJson(envelope, payload);

  // Sanity check the shape before the 400 KB body is assembled: a message without a
  // "role" makes the API reply 400 "Missing required parameter: 'messages[N].role'".
  {
    JsonDocument verification;
    if (deserializeJson(verification, payload) != DeserializationError::Ok) {
      Serial.println("ERROR: Request envelope is not valid JSON");
      return false;
    }
    JsonArray verify_messages = verification["messages"].as<JsonArray>();
    if (verify_messages.size() != 2 ||
        verify_messages[0]["role"].as<String>() != "system" ||
        verify_messages[1]["role"].as<String>() != "user" ||
        !verify_messages[1]["content"].is<JsonArray>()) {
      Serial.println("ERROR: Request envelope must be messages=[system, user] with a role on each");
      return false;
    }
  }

  int placeholder_pos = payload.indexOf(AUDIO_MODEL_B64_PLACEHOLDER);
  if (placeholder_pos < 0) {
    return false;
  }

  size_t placeholder_len = strlen(AUDIO_MODEL_B64_PLACEHOLDER);
  preamble = payload.substring(0, placeholder_pos);
  epilogue = payload.substring(placeholder_pos + placeholder_len);
  return true;
}

size_t base64_encoded_length(size_t raw_size) {
  return 4 * ((raw_size + 2) / 3);
}

String strip_json_code_fence(String text) {
  text.trim();
  if (!text.startsWith("```")) {
    return text;
  }

  int first_newline = text.indexOf('\n');
  if (first_newline < 0) {
    return text;
  }
  text = text.substring(first_newline + 1);
  int closing_fence = text.lastIndexOf("```");
  if (closing_fence >= 0) {
    text = text.substring(0, closing_fence);
  }
  text.trim();
  return text;
}

String parse_audio_model_response(const String& response, int http_response_code) {
  Serial.printf(">>> %s response code: %d\n", audio_model.c_str(), http_response_code);

  if (http_response_code <= 0) {
    Serial.printf("ERROR: HTTP request failed (code %d)\n", http_response_code);
    return "";
  }

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    Serial.println("ERROR: Failed to parse JSON response");
    Serial.println("Error: " + String(error.c_str()));
    Serial.println("Raw response: " + response);
    return "";
  }

  if (doc["error"].is<JsonObject>()) {
    String error_message = doc["error"]["message"] | "Unknown API error";
    String error_type = doc["error"]["type"] | "unknown";
    String error_code = doc["error"]["code"] | "unknown";
    Serial.println("OpenAI API Error (" + error_type + "/" + error_code + "): " + error_message);
    return "";
  }

  if (http_response_code != 200) {
    Serial.println("Raw response: " + response);
    return "";
  }

  if (doc["usage"].is<JsonObject>()) {
    int prompt_tokens = doc["usage"]["prompt_tokens"] | 0;
    int audio_tokens = doc["usage"]["prompt_tokens_details"]["audio_tokens"] | 0;
    int completion_tokens = doc["usage"]["completion_tokens"] | 0;
    Serial.printf(">>> Usage - prompt: %d (audio: %d), completion: %d\n",
                  prompt_tokens, audio_tokens, completion_tokens);
  }

  JsonVariant content = doc["choices"][0]["message"]["content"];
  String analysis = "";

  if (content.is<const char*>()) {
    analysis = content.as<String>();
  } else if (content.is<JsonArray>()) {
    for (JsonVariant part : content.as<JsonArray>()) {
      if (part["type"].as<String>() == "text" || part["text"].is<const char*>()) {
        analysis += part["text"].as<String>();
      }
    }
  }

  analysis = strip_json_code_fence(analysis);

  if (analysis.length() == 0) {
    Serial.println("ERROR: Could not find analysis content in JSON");
    Serial.println("Raw response: " + response);
    return "";
  }

  return analysis;
}

String analyze_wav_with_audio_model() {
  Serial.println("\n>>> STEP 2: Analyzing recorded audio with " + audio_model + " (single request)...");

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: WiFi not connected");
    return "";
  }

  if (!audio_buffer || audio_buffer_size == 0) {
    Serial.println("ERROR: No audio data available");
    return "";
  }

  if (ESP.getFreeHeap() < AUDIO_MODEL_MIN_FREE_HEAP) {
    Serial.println("ERROR: Not enough free heap for TLS connection");
    return "";
  }

  String preamble, epilogue;
  if (!build_audio_model_envelope(preamble, epilogue)) {
    Serial.println("ERROR: Failed to build the request envelope");
    return "";
  }

  size_t encoded_length = base64_encoded_length(audio_buffer_size);
  size_t total_size = preamble.length() + encoded_length + epilogue.length();

  Serial.printf(">>> WAV payload: %u bytes -> %u bytes base64 body (%.1f KB)\n",
                (unsigned)audio_buffer_size, (unsigned)total_size, total_size / 1024.0f);
  Serial.printf(">>> Free PSRAM: %u bytes, free heap: %u bytes\n",
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getFreeHeap());

  if (ESP.getFreePsram() < total_size + AUDIO_MODEL_PSRAM_MARGIN) {
    Serial.printf("ERROR: Not enough free PSRAM for the request body (need %u, have %u)\n",
                  (unsigned)(total_size + AUDIO_MODEL_PSRAM_MARGIN), (unsigned)ESP.getFreePsram());
    return "";
  }

  // +1 byte: mbedtls_base64_encode also writes a NUL terminator (and requires room for it),
  // that byte is later overwritten by the epilogue so the body stays contiguous
  uint8_t* request_body = (uint8_t*)heap_caps_malloc(total_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!request_body) {
    Serial.println("ERROR: Failed to allocate request body in PSRAM - record a shorter clip");
    return "";
  }

  memcpy(request_body, preamble.c_str(), preamble.length());

  size_t encoded_written = 0;
  int encode_result = mbedtls_base64_encode(request_body + preamble.length(),
                                            encoded_length + 1,
                                            &encoded_written,
                                            audio_buffer,
                                            audio_buffer_size);
  if (encode_result != 0 || encoded_written != encoded_length) {
    Serial.printf("ERROR: base64 encoding failed (code %d, wrote %u of %u bytes)\n",
                  encode_result, (unsigned)encoded_written, (unsigned)encoded_length);
    free(request_body);
    return "";
  }

  memcpy(request_body + preamble.length() + encoded_length, epilogue.c_str(), epilogue.length());

  WiFiClientSecure secure_client;
  secure_client.setInsecure();

  HTTPClient http;
  if (!http.begin(secure_client, openai_gpt_url)) {
    Serial.println("ERROR: Failed to initialize HTTP connection");
    free(request_body);
    return "";
  }

  http.setTimeout(AUDIO_MODEL_TIMEOUT_MS);
  http.setConnectTimeout(AUDIO_MODEL_CONNECT_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(openai_api_key));

  Serial.println(">>> Uploading audio (this can take 20-60 seconds)...");
  int http_response_code = http.POST(request_body, total_size);

  free(request_body);
  request_body = nullptr;
  Serial.printf(">>> Free PSRAM after upload: %u bytes\n", (unsigned)ESP.getFreePsram());

  String response = "";
  if (http_response_code > 0) {
    response = http.getString();
  }
  http.end();

  return parse_audio_model_response(response, http_response_code);
}

#endif
