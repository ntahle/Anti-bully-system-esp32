#ifndef CHATGPT_HANDLER_H
#define CHATGPT_HANDLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

extern const char* openai_gpt_url;
extern const char* openai_api_key;
extern String chatgpt_system_prompt;
extern String chatgpt_model;
extern String chatgpt_token;


String parseChatGPTResponse(String response, int httpResponseCode) {
  Serial.println("Response Code: " + String(httpResponseCode));
  //Serial.println("Full Response: " + response);  // Debug: see entire response

  DynamicJsonDocument doc(4096);
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
          // Serial.println("\n=== ChatGPT Response ===");
          // Serial.println(contentResponse);
          // Serial.println("========================\n");
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
  Serial.println("\n=== Sending to ChatGPT ===");
  Serial.println("Message: " + message);

  // Build JSON with ArduinoJson so quotes/newlines are escaped safely.
  DynamicJsonDocument payloadDoc(4096);
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

  http.begin(openai_gpt_url);
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
