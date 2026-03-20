#ifndef CHATGPT_HANDLER_H
#define CHATGPT_HANDLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

extern const char* openai_gpt_url;
extern const char* openai_api_key;
extern String chatgpt_system_prompt;
extern String chatgpt_model;
extern String chatgpt_token;

String extractAnalysisLabel(String analysis_result) {
  analysis_result.trim();

  // Prefer extracting the explicit JSON label field first.
  int labelKey = analysis_result.indexOf("\"label\"");
  if (labelKey >= 0) {
    int colonPos = analysis_result.indexOf(':', labelKey);
    int firstQuotePos = analysis_result.indexOf('"', colonPos + 1);
    int secondQuotePos = analysis_result.indexOf('"', firstQuotePos + 1);

    if (colonPos >= 0 && firstQuotePos >= 0 && secondQuotePos > firstQuotePos) {
      String label = analysis_result.substring(firstQuotePos + 1, secondQuotePos);
      label.toLowerCase();
      return label;
    }
  }

  // Fallback for plain-text responses.
  String lower = analysis_result;
  lower.toLowerCase();
  if (lower.indexOf("yes") >= 0) return "yes";
  if (lower.indexOf("uncertain") >= 0) return "uncertain";
  if (lower.indexOf("no") >= 0) return "no";
  return lower;
}

String buildSensorPayload(const String& transcription_text, const String& analysis_result) {
  DynamicJsonDocument outerDoc(1024);
  outerDoc["message"] = "Alert at Toilet L1";
  outerDoc["transcription"] = transcription_text;

  DynamicJsonDocument analysisDoc(768);
  DeserializationError analysisErr = deserializeJson(analysisDoc, analysis_result);
  if (!analysisErr) {
    outerDoc["analysis"] = analysisDoc.as<JsonVariant>();
  } else {
    // Fallback to plain text if model returns non-JSON output.
    outerDoc["analysis"] = analysis_result;
  }

  String payload;
  serializeJson(outerDoc, payload);
  return payload;
}

void printAnalysisResultPretty(const String& analysis_result) {
  DynamicJsonDocument analysisDoc(768);
  DeserializationError err = deserializeJson(analysisDoc, analysis_result);

  Serial.println("\n>>> Bully Analysis Result:");
  if (!err && analysisDoc.is<JsonObject>()) {
    String label = analysisDoc["label"] | "unknown";
    int confidence = analysisDoc["confidence"] | -1;

    Serial.println("- label: " + label);
    if (confidence >= 0) {
      Serial.println("- confidence: " + String(confidence));
    }

    if (analysisDoc["reasons"].is<JsonArray>()) {
      JsonArray reasons = analysisDoc["reasons"].as<JsonArray>();
      if (reasons.size() > 0) {
        Serial.println("- reasons:");
        for (JsonVariant reason : reasons) {
          Serial.println("  * " + reason.as<String>());
        }
      }
    }
    return;
  }

  // Fallback for non-JSON responses.
  Serial.println(analysis_result);
}

String parseChatGPTResponse(String response, int httpResponseCode) {
  Serial.println("Response Code: " + String(httpResponseCode));
  //Serial.println("Full Response: " + response);  // Debug: see entire response

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, response);
  
  if (error) {
    Serial.println("ERROR: Failed to parse JSON");
    Serial.println("Error: " + String(error.c_str()));
    Serial.println("Raw response: " + response);
    return "";
  }

  if (doc.containsKey("error")) {
    JsonObject err = doc["error"];
    String errMsg = err["message"] | "Unknown API error";
    String errType = err["type"] | "unknown";
    Serial.println("OpenAI API Error (" + errType + "): " + errMsg);
    return "";
  }

  // Navigate through the JSON structure: choices[0].message.content
  if (doc.containsKey("choices") && doc["choices"].is<JsonArray>()) {
    JsonArray choices = doc["choices"];
    if (choices.size() > 0) {
      JsonObject firstChoice = choices[0];
      if (firstChoice.containsKey("message")) {
        JsonObject message = firstChoice["message"];
        if (message.containsKey("content")) {
          String contentResponse = message["content"].as<String>();
          return contentResponse;
        }
      }
    }
  }
  
  Serial.println("ERROR: Could not find response content in JSON");
  return "";
}

String sendToChatGPT(String message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("ERROR: Not connected to WiFi. Cannot send to ChatGPT.");
    return "";
  }

  HTTPClient http;
  WiFiClientSecure secure_client;
  secure_client.setInsecure();

  Serial.println("\n=== Sending to ChatGPT ===");
  Serial.println("Message: " + message);
  Serial.printf("Free heap before ChatGPT HTTPS: %u bytes\n", ESP.getFreeHeap());

  // Build JSON with ArduinoJson so quotes/newlines are escaped safely.
  DynamicJsonDocument payloadDoc(2048);
  payloadDoc["model"] = chatgpt_model;
  payloadDoc["max_tokens"] = chatgpt_token.toInt();
  payloadDoc["temperature"] = 0.2;

  JsonArray messages = payloadDoc.createNestedArray("messages");
  JsonObject systemMsg = messages.createNestedObject();
  systemMsg["role"] = "system";
  systemMsg["content"] = chatgpt_system_prompt;

  JsonObject userMsg = messages.createNestedObject();
  userMsg["role"] = "user";
  userMsg["content"] = message;

  String payload;
  serializeJson(payloadDoc, payload);

  http.begin(secure_client, openai_gpt_url);
  http.setTimeout(30000);
  http.setConnectTimeout(10000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(openai_api_key));

  Serial.println("Connecting to OpenAI API...");
  int httpResponseCode = http.POST(payload);

  String contentResponse = "";
  if (httpResponseCode > 0) {
    String response = http.getString();
    contentResponse = parseChatGPTResponse(response, httpResponseCode);
  } else {
    Serial.println("ERROR: HTTP Request failed!");
    Serial.println("Error Code: " + String(httpResponseCode));
    String errorResponse = http.getString();
    Serial.println("Error Response: " + errorResponse);
  }

  http.end();
  return contentResponse;
}

// Main function to analyze text for bullying behavior
String analyze_text_with_chatgpt(String text) {
  Serial.println("\n>>> STEP 3: Analyzing text with ChatGPT...");
  String analysis_result = sendToChatGPT(text);
  return analysis_result;
}

#endif // CHATGPT_HANDLER_H
