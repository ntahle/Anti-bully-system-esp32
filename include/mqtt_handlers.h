#ifndef MQTT_HANDLERS_H
#define MQTT_HANDLERS_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <base64.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>


extern const char* emqx_api;  
extern const char* app_id;                 
extern const char* app_secret;
extern const char* mqtt_username;
extern const char* mqtt_password;
extern const char* topic_status;
extern const char* topic_ack;

WiFiClientSecure mqtt_tls_client;
PubSubClient mqtt_client(mqtt_tls_client);
String mqtt_broker_host;
bool mqtt_ack_paused = false;
volatile bool mqtt_ack_pressed_flag = false;

String extractBrokerHost(const String& apiUrl) {
  String host = apiUrl;

  int schemePos = host.indexOf("://");
  if (schemePos >= 0) {
    host = host.substring(schemePos + 3);
  }

  int slashPos = host.indexOf('/');
  if (slashPos >= 0) {
    host = host.substring(0, slashPos);
  }

  int portPos = host.indexOf(':');
  if (portPos >= 0) {
    host = host.substring(0, portPos);
  }

  host.trim();
  return host;
}

void mqttMessageCallback(char* topic, byte* payload, unsigned int length) {
  String incomingTopic(topic);
  String payloadText;
  payloadText.reserve(length + 1);

  for (unsigned int i = 0; i < length; i++) {
    payloadText += (char)payload[i];
  }

  if (incomingTopic != topic_ack) {
    return;
  }

  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, payloadText);
  if (err) {
    Serial.printf("MQTT ack parse error: %s\n", err.c_str());
    return;
  }

  bool acknowledged = doc["acknowledged"] | false;
  if (acknowledged) {
    mqtt_ack_pressed_flag = true;
  }
}

bool consumeAcknowledgedPressed() {
  if (!mqtt_ack_pressed_flag) {
    return false;
  }

  mqtt_ack_pressed_flag = false;
  return true;
}

bool ensureMqttConnected() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  if (mqtt_broker_host.length() == 0) {
    mqtt_broker_host = extractBrokerHost(String(emqx_api));
    if (mqtt_broker_host.length() == 0) {
      Serial.println("MQTT broker host is empty");
      return false;
    }
    mqtt_tls_client.setInsecure();
    mqtt_client.setServer(mqtt_broker_host.c_str(), 8883);
    mqtt_client.setCallback(mqttMessageCallback);
  }

  if (mqtt_client.connected()) {
    return true;
  }

  String clientId = "esp32-ack-" + String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFF), HEX);
  bool connected = mqtt_client.connect(clientId.c_str(), mqtt_username, mqtt_password);
  if (!connected) {
    Serial.printf("MQTT connect failed, rc=%d\n", mqtt_client.state());
    return false;
  }

  if (!mqtt_client.subscribe(topic_ack, 1)) {
    Serial.println("MQTT subscribe failed for ack topic");
    return false;
  }

  Serial.print("Subscribed to topic: ");
  Serial.println(topic_ack);
  return true;
}

void serviceMqttAck() {
  if (mqtt_ack_paused) {
    return;
  }

  if (ensureMqttConnected()) {
    mqtt_client.loop();
  }
}

void pauseMqttAck() {
  mqtt_ack_paused = true;
  if (mqtt_client.connected()) {
    mqtt_client.disconnect();
  }
  mqtt_tls_client.stop();
}

void resumeMqttAck() {
  mqtt_ack_paused = false;
}

bool waitForMqttAckReady(uint32_t timeoutMs) {
  unsigned long start = millis();

  while (millis() - start < timeoutMs) {
    if (ensureMqttConnected()) {
      mqtt_client.loop();
      return true;
    }
    delay(20);
  }

  return false;
}




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

bool publishViaAPI(String topic, String payload, int qos = 0, bool verbose = true) {
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
  String jsonPayload = "{\"topic\":\"" + escapedTopic + "\",\"payload\":\"" + escapedPayload + "\",\"qos\":" + String(qos) + ",\"retain\":false}";
  
  int httpCode = http.POST(jsonPayload);
  
  if (httpCode > 0) {
    if (verbose) {
      Serial.printf("HTTP Response: %d\n", httpCode);
      String response = http.getString();
      Serial.println(response);
    }
    http.end();
    return (httpCode == 200 || httpCode == 204);
  } else {
    if (verbose) {
      Serial.printf("HTTP Error: %s\n", http.errorToString(httpCode).c_str());
    }
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
  publishViaAPI(topic_status, statusPayload, 0, false);
}

#endif  // MQTT_HANDLERS_H