#ifndef MQTT_HANDLERS_H
#define MQTT_HANDLERS_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include <ArduinoJson.h>


extern const char* emqx_api;  
extern const char* app_id;                 
extern const char* app_secret;
extern const char* mqtt_username;
extern const char* mqtt_password;
extern const char* topic_status;




String jsonEscape(const String& input) {
  String output;
  output.reserve(input.length() + 8);

  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '\\' || c == '"') {
      output += '\\';
      output += c;
    } else if (c == '\n') {
      output += "\\n";
    } else if (c == '\r') {
      output += "\\r";
    } else if (c == '\t') {
      output += "\\t";
    } else {
      output += c;
    }
  }

  return output;
}

bool publishViaAPI(String topic, String payload) {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  // Create secure client for HTTPS
  WiFiClientSecure client;
  client.setInsecure();  // Skip SSL certificate verification (for testing)
  
  HTTPClient http;
  
  // EMQX Serverless API endpoint for publishing
  String url = String(emqx_api) + "/publish";
  http.begin(client, url);  // Pass the secure client
  
  // Set headers
  http.addHeader("Content-Type", "application/json");
  
  // Use App ID and Secret for authentication
  String auth = String(app_id) + ":" + String(app_secret);
  String authBase64 = base64::encode((uint8_t*)auth.c_str(), auth.length());
  http.addHeader("Authorization", "Basic " + authBase64);
  
  // Escape dynamic fields so nested JSON/text remains valid in request body.
  String escapedTopic = jsonEscape(topic);
  String escapedPayload = jsonEscape(payload);
  String jsonPayload = "{\"topic\":\"" + escapedTopic + "\",\"payload\":\"" + escapedPayload + "\",\"qos\":0,\"retain\":false}";
  
  int httpCode = http.POST(jsonPayload);
  
  if (httpCode > 0) {
    Serial.printf("HTTP Response: %d\n", httpCode);
    String response = http.getString();
    Serial.println(response);
    http.end();
    return (httpCode == 200 || httpCode == 204);
  } else {
    Serial.printf("HTTP Error: %s\n", http.errorToString(httpCode).c_str());
    http.end();
    return false;
  }
}

void publishEsp32Status(const char* state) {
  StaticJsonDocument<160> statusDoc;
  statusDoc["device"] = "esp32";
  statusDoc["status"] = state;
  statusDoc["uptime_ms"] = millis();

  String statusPayload;
  serializeJson(statusDoc, statusPayload);
  publishViaAPI(topic_status, statusPayload);
}

#endif  // MQTT_HANDLERS_H