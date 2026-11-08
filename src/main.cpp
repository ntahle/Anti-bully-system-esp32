#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <String>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "handlers.h" //wifi config handlers
#include "chatgpt_handler.h" //chatgpt analyser
#include "audio_recorder.h" //mic 
#include "openai_whisper.h" //speech to text
#include "audio_analysis.h" //one-shot WAV -> analysis (gpt-audio-1.5)
#include "wav_generator.h" //generate wav file in RAM
#include "telegram_handler.h" //telegram bot notification
#include "UniversalTelegramBot.h"
#include "mqtt_handlers.h" //mqtt publish handlers
#include "firebase_handler.h" //fcm publish handler
#if __has_include("secrets.h")
#include "secrets.h" //local credentials
#else
#include "secrets.template.h" //placeholder credentials for compilation
#endif

int led = 6; 
int pb = 8;
const int ledBlink = 5;
const int bell = 7;

const int bellActiveLevel = LOW;
const int bellInactiveLevel = HIGH;

const unsigned long micTriggerCooldownMs = 10000;
unsigned long lastMicTriggerTime = 0;

unsigned long pbPressStart = 0;
bool pbResetHandled = false;
unsigned long lastWiFiCheck = 0;
unsigned long lastPbClickTime = 0;
unsigned long lastPbDebounceTime = 0;
bool lastPbState = false;
int pbClickCount = 0;
const unsigned long pbDebounceMs = 50;
const unsigned long pbDoubleClickWindowMs = 1500;


const char* openai_api_key = SECRET_OPENAI_API_KEY;
const char* openai_whisper_url = "https://api.openai.com/v1/audio/transcriptions";
const char* whisper_model = "gpt-4o-transcribe-diarize";
const char* openai_gpt_url = "https://api.openai.com/v1/chat/completions";
String chatgpt_model = "gpt-4o-mini";
String chatgpt_token = "100";
String audio_model = "gpt-audio-1.5"; //audio-input chat model for the single-request pipeline


const char* botToken = SECRET_TELEGRAM_BOT_TOKEN;
const char* chatID = SECRET_TELEGRAM_CHAT_ID;

const char* emqx_api = SECRET_EMQX_API;  
const char* app_id = "h2bf1171";                 
const char* app_secret = SECRET_EMQX_APP_SECRET;
const char* mqtt_username = "syafiq";
const char* mqtt_password = SECRET_MQTT_PASSWORD;
const char* topic_subs = "testtopic/esp32";
const char* topic_status = "testtopic/esp32/status";
const char* topic_ack = "testtopic/acknowledgment";
const char* topic_alert = "testtopic/esp32/alert";

const char* fcm_api_url = SECRET_FCM_API_URL;
const char* fcm_device_token = SECRET_FCM_DEVICE_TOKEN;
const char* firebase_token_url = "https://vmc-api.edxapay.com/firebase-show";

String chatgpt_system_prompt = R"PROMPT(You are a school safety triage classifier for elementary and secondary schools in Malaysia.
Task: decide whether the speech in the audio shows that a student needs immediate help or is exposed to bullying, threat, fear, coercion, harassment, sexual harassment, sexual assault or other violence, and name the category.

Return JSON only with keys:
- label: one of ["yes","no","uncertain"]
- category: one of ["bullying","sexual_harassment","sexual_violence","other_safety","none"]
- confidence: integer 0-100
- reasons: array of up to 3 short phrases
- transcript: verbatim transcript of what was said in the audio, empty string if nothing intelligible

Rules:
- "yes" if there is a direct request for help, fear of harm, threat, coercion, repeated harassment, unwanted sexual attention or touching, any sexual act done to the student without consent, or urgent distress.
- category = the most serious issue present. If sexual and bullying elements appear together, use "sexual_harassment" or "sexual_violence", not "bullying".
- sexual_harassment: unwanted sexual comments, jokes, gestures, messages, images, exposure, or touching without penetration. This is "yes" even when the students sound casual, embarrassed, or are laughing it off.
- Treat unwanted physical contact as sexual_harassment even when no sexual words are used: touching, grabbing, groping, rubbing, cornering, forced kissing or hugging (pegang, raba, gesel, cium, peluk paksa), unless it is clearly accidental or a normal part of sports or medical care.
- sexual_violence: rape, attempted rape, molestation, forced or coerced sexual contact, or any sexual act involving a minor. Always "yes" with confidence 90 or above.
- Never dismiss sexual content because it is described with slang or euphemism, or because the student talks about someone else in the third person. Words such as rogol, kacau, usik, pegang, paksa or similar Malay terms count.
- "no" if casual talk, jokes, teasing, or unrelated school chatter with no distress and no sexual content.
- "uncertain" if the transcription is noisy, incomplete, contradictory, or too short to judge.
- Be strict about harmless chatter: do not over-trigger on ordinary teasing.
- The speech may be English, Malay, or a mix; focus on the meaning rather than specific words.
- Do not include any text outside JSON.)PROMPT";

WebServer server(80);
WiFiClientSecure client;

i2s_port_t i2s_port = I2S_NUM_0;
bool wifi_connected = false;
String last_transcription = "";
String transcription = "";
bool isPressed = false;
uint8_t* audio_buffer = nullptr;
size_t audio_buffer_size = 0;
bool recording_active;
unsigned long lastStatusPublish = 0;


void blinkLed13(int times, int delayMs) {
  for (int i = 0; i < times; i++) {
    digitalWrite(ledBlink, HIGH);
    delay(delayMs);
    digitalWrite(ledBlink, LOW);
    delay(delayMs);
  }
}

bool bellRinging = false;
bool bellOutputOn = false;
unsigned long bellRingStart = 0;
unsigned long bellLastToggle = 0;

const unsigned long bellOnMs = 10000;
const unsigned long bellOffMs = 10000;      
const unsigned long bellMaxRingMs = 60000;

void bellWriteOutput(bool on) {
  digitalWrite(bell, on ? bellActiveLevel : bellInactiveLevel);
  bellOutputOn = on;
}

void bellOn() {
  bellWriteOutput(true);
  bellRinging = true;
  bellRingStart = millis();
  bellLastToggle = millis();
}

void bellOff() {
  bellWriteOutput(false);
  bellRinging = false;
}

void serviceBellPattern() {
  if (!bellRinging) {
    return;
  }

  unsigned long stepMs = bellOutputOn ? bellOnMs : bellOffMs;
  if (millis() - bellLastToggle < stepMs) {
    return;
  }

  bellLastToggle = millis();
  bellWriteOutput(!bellOutputOn);
  Serial.print(">>> Bell pattern: ");
  Serial.println(bellOutputOn ? "ON" : "OFF");
}



void setup() {

  digitalWrite(bell, bellInactiveLevel);
  pinMode(bell, OUTPUT);

  Serial.begin(115200);

  pinMode(led, OUTPUT);
  pinMode(ledBlink, OUTPUT); 
  pinMode(pb, INPUT);
  
  bellWriteOutput(false);
   
  EEPROM.begin(EEPROM_SIZE);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("ESP32_ABS", "12345678");
  
  connectToWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    publishEsp32Status("online");
    lastStatusPublish = millis();
  }

  if (!init_i2s_inmp441()) {
    Serial.println("I2S initialization failed!");
    while (1) delay(1000);
  }
  
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset", HTTP_POST, handleReset);
  
  // Captive portal handlers
  server.on("/generate_204", handleCaptivePortal);
  server.on("/success.txt", handleCaptivePortal);
  server.on("/hotspot-detect.html", handleCaptivePortal);
  server.on("/canonical.html", handleCaptivePortal);
  server.on("/mobile/status.php", handleCaptivePortal);
  server.on("/api/online", handleCaptivePortal);
  
  server.onNotFound(handleNotFound);
  server.begin();
  teleBegin();
  
  Serial.println("Access Point started: ESP32_ABS");
  Serial.println("\nSystem ready!");
}

void loop() {
  server.handleClient();
  bool pbState = (digitalRead(pb) == HIGH);

  if (pbState != lastPbState && (millis() - lastPbDebounceTime) > pbDebounceMs) {
    lastPbDebounceTime = millis();
    lastPbState = pbState;

    if (pbState) {
      if (lastPbClickTime != 0 && (millis() - lastPbClickTime) <= pbDoubleClickWindowMs) {
        pbClickCount++;
      } else {
        pbClickCount = 1;
      }

      lastPbClickTime = millis();

      if (pbClickCount >= 2) {
        
        bool fcmSent = sendFcmNotification("Testing connection from ESP32", "testing", "ABS Test Notification");

        if (fcmSent) {
          Serial.println(">>> Sent: FCM testing notification");
        } else {
          Serial.println(">>> Failed to send: FCM testing notification");
        }
        pbClickCount = 0;
        lastPbClickTime = 0;
      }
    }
  }

  if (lastPbClickTime != 0 && (millis() - lastPbClickTime) > pbDoubleClickWindowMs) {
    pbClickCount = 0;
    lastPbClickTime = 0;
  }

  if (WiFi.status() == WL_CONNECTED ) Connected();
  else Disconnected();

  serviceMqttAck();
  if (consumeAcknowledgedPressed()) {
    Serial.println("acknowledged pressed");
    bellOff();
    i2s_zero_dma_buffer(i2s_port);  
    blinkLed13(2, 200);   
  }

  if (bellRinging && bellMaxRingMs > 0 && millis() - bellRingStart >= bellMaxRingMs) {
    Serial.println(">>> Bell ring timeout reached, silencing bell");
    bellOff();
  }

  serviceBellPattern(); 

  if (WiFi.status() == WL_CONNECTED && millis() - lastStatusPublish >= 2000) {
    publishEsp32Status("online");
    lastStatusPublish = millis();
  }

  if (pbState) {
    if (pbPressStart == 0) {
      pbPressStart = millis();
    }

    if (!pbResetHandled && (millis() - pbPressStart >= 3000)) {
      handleReset();
      pbResetHandled = true;
    }
  } else {
    pbPressStart = 0;
    pbResetHandled = false;
  }

  if (!bellRinging) {
    bool micTriggered = detect_microphone_activity();
    if (WiFi.status() == WL_CONNECTED && micTriggered && (millis() - lastMicTriggerTime >= micTriggerCooldownMs)) {
      lastMicTriggerTime = millis();
      
      Serial.println("\n========================================");
      Serial.println(">>> Microphone activity detected, starting workflow");
      digitalWrite(ledBlink, HIGH); 
      pauseMqttAck();
  
      record_audio_from_microphone();

#if SINGLE_PIPELINE_AUDIO_MODEL
      // Single request: recorded WAV -> analysis JSON (transcript comes back in the same response)
      String analysis_result = analyze_wav_with_audio_model();
      free_audio_memory();

      String transcribed_text = "";
      if (analysis_result.length() > 0) {
        JsonDocument analysis_doc;
        if (deserializeJson(analysis_doc, analysis_result) == DeserializationError::Ok) {
          transcribed_text = analysis_doc["transcript"].as<String>();
        }
      }
#else
      // Two separate requests: speech to text, then text analysis
      String transcribed_text = convert_audio_to_text();
      free_audio_memory();

      String analysis_result = "";
      if (transcribed_text.length() > 0) {
        analysis_result = analyze_text_with_chatgpt(transcribed_text);
      }
#endif

      if (analysis_result.length() > 0) {
        String analysis_label = extractAnalysisLabel(analysis_result);
        String analysis_category = extractAnalysisCategory(analysis_result);
        String alert_headline = buildAlertHeadline(analysis_category);
        String alert_title = buildAlertTitle(analysis_category);
        printAnalysisResultPretty(analysis_result);
        if (analysis_label == "yes") {
          
          Serial.println("\n\n>>> ALERT: " + alert_headline + "!");
          bot.sendMessage(chatID, "ALERT: " + alert_headline + " in the latest transcription:\n\n\"" + transcribed_text + "\"\n\nAnalysis: " + analysis_result);
          
          String sensorData = buildSensorPayload(transcribed_text, analysis_result);
          
          String transcriptionPreview = transcribed_text;
          transcriptionPreview.replace("\n", " ");
          if (transcriptionPreview.length() > 120) {
            transcriptionPreview = transcriptionPreview.substring(0, 120) + "...";
          }
          String alertBody = "ALERT: " + alert_headline;
          if (transcriptionPreview.length() > 0) {
            alertBody += " - \"" + transcriptionPreview + "\"";
          }
          bool fcmSent = sendFcmNotification(alertBody, "alert", alert_title, sensorData);
          bool mqttAlertSent = publishAlert(alert_title, alertBody, sensorData);

          resumeMqttAck();
          bellOn();
          
          if (!waitForMqttAckReady(1500)) {
            Serial.println(">>> WARNING: ACK listener not ready yet");
          }
          if (fcmSent) {
            Serial.println(">>> " + alert_headline + " - push notification sent.");
          } else {
            Serial.println(">>> Failed to send alert push notification.");
          }
          if (mqttAlertSent) {
            Serial.println(">>> " + alert_headline + " - published to MQTT.");
          } else {
            Serial.println(">>> Failed to publish alert to MQTT.");
          }


        } else {
          Serial.println(">>> No bullying, harassment or violence detected.");
        }
      } else {
        Serial.println(">>> Skipping analysis - no analysis result available");
      }
  
      resumeMqttAck();
      digitalWrite(ledBlink, LOW); 
    }
  }
  delay(50);
  
}
