#ifndef TELEGRAM_HANDLER_H
#define TELEGRAM_HANDLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

extern const char* botToken;
extern const char* chatID;
extern uint8_t* audio_buffer;
extern size_t audio_buffer_size;

extern WiFiClientSecure client;
UniversalTelegramBot bot(botToken, client);



void teleBegin(){
    client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
    Serial.println("Telegram Bot Initialized");
    bot.sendMessage(chatID, "ESP32 Bot is now online!");
}

// Send audio file to Telegram
bool send_audio_to_telegram() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: WiFi not connected. Cannot send to Telegram.");
    return false;
  }

  if (!audio_buffer || audio_buffer_size == 0) {
    Serial.println("ERROR: No audio data to send");
    return false;
  }

  Serial.println(">>> Sending audio file to Telegram...");
  Serial.printf("Audio size: %d bytes\n", audio_buffer_size);

  HTTPClient http;
  String telegram_url = "https://api.telegram.org/bot" + String(botToken) + "/sendAudio";

  if (!http.begin(telegram_url)) {
    Serial.println("ERROR: Failed to connect to Telegram");
    return false;
  }

  http.setTimeout(30000);

  // Build multipart form data
  String boundary = "----TelegramBoundary123456789";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  String body_start = "--" + boundary + "\r\n";
  body_start += "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n";
  body_start += String(chatID);
  body_start += "\r\n";
  body_start += "--" + boundary + "\r\n";
  body_start += "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n";
  body_start += "Content-Type: audio/wav\r\n\r\n";

  String body_end = "\r\n--" + boundary + "--\r\n";

  size_t start_len = body_start.length();
  size_t end_len = body_end.length();
  size_t total_size = start_len + audio_buffer_size + end_len;

  Serial.printf("Total request size: %d bytes\n", total_size);
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());

  // Check if we can reuse the audio buffer
  if (total_size > 110000) {
    Serial.println("ERROR: Request exceeds buffer size!");
    http.end();
    return false;
  }

  // REUSE audio_buffer - shift audio data to make room for headers
  // Move audio data forward to make space for body_start
  memmove(audio_buffer + start_len, audio_buffer, audio_buffer_size);
  // Copy header at the beginning
  memcpy(audio_buffer, body_start.c_str(), start_len);
  // Copy footer at the end
  memcpy(audio_buffer + start_len + audio_buffer_size, body_end.c_str(), end_len);

  Serial.println("Sending HTTP POST request to Telegram...");
  int httpResponseCode = http.POST(audio_buffer, total_size);

  if (httpResponseCode == 200) {
    Serial.println(">>> Audio sent to Telegram successfully!");
    http.end();
    return true;
  } else {
    Serial.printf(">>> Telegram Error: HTTP %d\n", httpResponseCode);
    String response = http.getString();
    Serial.println("Response: " + response);
    http.end();
    return false;
  }
}

// Step 4: Send audio to Telegram (called from main.cpp)
void backup_audio_to_telegram() {
  Serial.println("\n>>> STEP 4: Backing up audio to Telegram...");
  
  if (send_audio_to_telegram()) {
    Serial.println(">>> Audio backup successful!");
  } else {
    Serial.println(">>> Audio backup failed (continuing anyway)");
  }
}

#endif // TELEGRAM_HANDLER_H