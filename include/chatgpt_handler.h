#ifndef CHATGPT_HANDLER_H
#define CHATGPT_HANDLER_H

#include <Arduino.h>
#include <ArduinoJson.h>

extern const char* openai_gpt_url;
extern const char* openai_api_key;
extern String chatgpt_system_prompt;

String extractAnalysisLabel(String analysis_result) {
  analysis_result.trim();

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

  String lower = analysis_result;
  lower.toLowerCase();
  if (lower.indexOf("yes") >= 0) return "yes";
  if (lower.indexOf("uncertain") >= 0) return "uncertain";
  if (lower.indexOf("no") >= 0) return "no";
  return lower;
}

// Human readable alert wording for a detected category. The default keeps the original
// bullying wording so existing notifications stay unchanged.
String buildAlertHeadline(const String& category) {
  if (category == "sexual_harassment") return "Sexual harassment detected";
  if (category == "sexual_violence") return "Sexual violence or rape detected";
  if (category == "other_safety") return "Safety concern detected";
  return "Bullying behavior detected";
}

String buildAlertTitle(const String& category) {
  if (category == "sexual_harassment") return "Sexual Harassment Alert Detected";
  if (category == "sexual_violence") return "Sexual Violence Alert Detected";
  if (category == "other_safety") return "Safety Alert Detected";
  return "Bullying Alert Detected";
}

String extractAnalysisCategory(const String& analysis_result) {
  int categoryKey = analysis_result.indexOf("\"category\"");
  if (categoryKey >= 0) {
    int colonPos = analysis_result.indexOf(':', categoryKey);
    int firstQuotePos = analysis_result.indexOf('"', colonPos + 1);
    int secondQuotePos = analysis_result.indexOf('"', firstQuotePos + 1);

    if (colonPos >= 0 && firstQuotePos >= 0 && secondQuotePos > firstQuotePos) {
      String category = analysis_result.substring(firstQuotePos + 1, secondQuotePos);
      category.toLowerCase();
      return category;
    }
  }

  return "";
}

String buildSensorPayload(const String& transcription_text, const String& analysis_result) {
  DynamicJsonDocument outerDoc(1536);
  outerDoc["message"] = "Alert at Fukuyama Dorm DML114 L1";
  outerDoc["transcription"] = transcription_text;

  DynamicJsonDocument analysisDoc(1024);
  DeserializationError analysisErr = deserializeJson(analysisDoc, analysis_result);
  if (!analysisErr) {
    outerDoc["analysis"] = analysisDoc.as<JsonVariant>();
  } else {
    outerDoc["analysis"] = analysis_result;
  }

  String payload;
  serializeJson(outerDoc, payload);
  return payload;
}

void printAnalysisResultPretty(const String& analysis_result) {
  DynamicJsonDocument analysisDoc(1024);
  DeserializationError err = deserializeJson(analysisDoc, analysis_result);

  Serial.println("\n\n>>> Safety Analysis Result:");
  if (!err && analysisDoc.is<JsonObject>()) {
    String label = analysisDoc["label"] | "unknown";
    int confidence = analysisDoc["confidence"] | -1;

    Serial.println("- label: " + label);
    String category = analysisDoc["category"] | "";
    if (category.length() > 0 && category != "none") {
      Serial.println("- category: " + category);
    }
    if (confidence >= 0) {
      Serial.println("- confidence: " + String(confidence));
    }

    if (analysisDoc["transcript"].is<const char*>()) {
      String transcript = analysisDoc["transcript"].as<String>();
      if (transcript.length() > 0) {
        Serial.println("- transcript: \"" + transcript + "\"");
      }
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

  Serial.println(analysis_result);
}

#endif // CHATGPT_HANDLER_H
