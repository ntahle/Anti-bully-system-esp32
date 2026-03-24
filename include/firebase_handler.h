#ifndef FIREBASE_HANDLER_H
#define FIREBASE_HANDLER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <HTTPClient.h>

extern const char* fcm_api_url;
extern const char* fcm_auth_bearer;
extern const char* fcm_device_token;


bool sendFcmNotification(const String& messageBody, const String& notificationType = "testing", const String& notificationTitle = "Anti-Bullying System", const String& mqttPayloadJson = "") {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(">>> FCM skipped: WiFi not connected");
    return false;
  }

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  HTTPClient http;
  if (!http.begin(secureClient, fcm_api_url)) {
    Serial.println(">>> FCM failed: cannot initialize HTTP client");
    return false;
  }

  http.setTimeout(15000);
  http.setConnectTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + fcm_auth_bearer);

  JsonDocument doc;
  JsonObject message = doc["message"].to<JsonObject>();
  message["token"] = fcm_device_token;

  JsonObject notification = message["notification"].to<JsonObject>();
  notification["title"] = notificationTitle;
  notification["body"] = messageBody;

  JsonObject data = message["data"].to<JsonObject>();
  data["source"] = "esp32";
  data["type"] = notificationType;

  bool mappedFromMqttPayload = false;
  if (mqttPayloadJson.length() > 0) {
    JsonDocument mqttDoc;
    DeserializationError err = deserializeJson(mqttDoc, mqttPayloadJson);
    if (!err && mqttDoc.is<JsonObject>()) {
      if (mqttDoc["message"].is<const char*>()) {
        data["message"] = String((const char*)mqttDoc["message"]);
      }

      if (mqttDoc["transcription"].is<const char*>()) {
        data["transcription"] = String((const char*)mqttDoc["transcription"]);
      }

      if (!mqttDoc["analysis"].isNull()) {
        String analysisString;
        serializeJson(mqttDoc["analysis"], analysisString);
        data["analysis"] = analysisString;
      }

      data["mqtt_payload"] = mqttPayloadJson;
      mappedFromMqttPayload = true;
    }
  }

  if (!mappedFromMqttPayload) {
    data["message"] = messageBody;
  }

  String payload;
  serializeJson(doc, payload);

  int httpCode = http.POST(payload);
  String response = http.getString();
  http.end();

  if (httpCode > 0 && (httpCode == 200 || httpCode == 201)) {
    Serial.printf(">>> FCM sent successfully (HTTP %d)\n", httpCode);
    return true;
  }

  Serial.printf(">>> FCM failed (HTTP %d): %s\n", httpCode, response.c_str());
  return false;
}

#endif