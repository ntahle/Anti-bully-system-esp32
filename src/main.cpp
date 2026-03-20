#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <String>
#include <ArduinoJson.h>
#include "handlers.h" //wifi config handlers
#include "chatgpt_handler.h" //chatgpt analyser
#include "audio_recorder.h" //mic 
#include "openai_whisper.h" //speech to text
#include "wav_generator.h" //generate wav file in RAM
#include "telegram_handler.h" //telegram bot notification
#include "UniversalTelegramBot.h"
#include "mqtt_handlers.h" //mqtt publish handlers
#if __has_include("secrets.h")
#include "secrets.h" //local credentials
#else
#include "secrets.template.h" //placeholder credentials for compilation
#endif

int led = 2; 
int pb = 4;
int pb1 = 15;
int pbcond = 0;
const int ledBlink = 13;

unsigned long lastPB;
unsigned long lastWiFiCheck = 0;

const char* openai_api_key = SECRET_OPENAI_API_KEY;
const char* openai_whisper_url = "https://api.openai.com/v1/audio/transcriptions";
const char* whisper_model = "gpt-4o-transcribe-diarize";
const char* openai_gpt_url = "https://api.openai.com/v1/chat/completions";
String chatgpt_model = "gpt-4o-mini";
String chatgpt_token = "100";


const char* botToken = SECRET_TELEGRAM_BOT_TOKEN;
const char* chatID = SECRET_TELEGRAM_CHAT_ID;

const char* emqx_api = SECRET_EMQX_API;  
const char* app_id = "m06f1f17";                 
const char* app_secret = SECRET_EMQX_APP_SECRET;
const char* mqtt_username = "syafiq";
const char* mqtt_password = SECRET_MQTT_PASSWORD;
const char* topic_subs = "testtopic/esp32";
const char* topic_status = "testtopic/esp32/status";
const char* topic_ack = "testtopic/acknowledgment";

String chatgpt_system_prompt = R"PROMPT(You are a school safety triage classifier in Malaysia's school in elementary and secondary levels.
Task: classify whether the student text indicates immediate help-seeking or probability which lead to possible bully incidents which related to bullying, threat, fear, coercion, harassment, or violence.
Return JSON only with keys:
- label: one of ["yes","no","uncertain"]
- confidence: integer 0-100
- reasons: array of up to 3 short phrases

Rules:
- "yes" if there is direct request for help, fear of harm, threat, coercion, repeated harassment, or urgent distress.
- "no" if casual talk, jokes, unrelated school chatter, or unclear context without distress.
- "uncertain" if transcription is noisy, incomplete, contradictory, or too short.
- Be strict: do not over-trigger.
- the conversation may contain slang, misspellings, or informal language common among students. Consider these in your analysis.
- the conversation may in english or a mix of english, Malay and local language. Focus on the meaning rather than specific words.
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

void setup() {
  Serial.begin(115200);
  pinMode(led, OUTPUT);
  pinMode(ledBlink, OUTPUT); 
   
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
  int pbState = digitalRead(pb);
  int pushNoti = digitalRead(pb1);


  if (WiFi.status() == WL_CONNECTED ) Connected();
  else Disconnected();

  serviceMqttAck();
  if (consumeAcknowledgedPressed()) {
    Serial.println("acknowledged pressed");
    blinkLed13(2, 200);
  }

  if (WiFi.status() == WL_CONNECTED && millis() - lastStatusPublish >= 10000) {
    publishEsp32Status("online");
    lastStatusPublish = millis();
  }

  if (pbState == LOW) {
    lastPB = millis();
  }
  if (millis() - lastPB > 3000 && pbState == HIGH && lastPB != 0) {
    handleReset();
    lastPB = 0; 
  }

  if (pushNoti == HIGH && pbcond == 0) { //system triggered
    
    Serial.println("\n========================================");
    digitalWrite(ledBlink, HIGH); 
    pauseMqttAck();

    record_audio_from_microphone();
    
    String transcribed_text = convert_audio_to_text(); // Step 2: Convert audio to text
    free_audio_memory(); // Free RAM before next HTTPS request (ChatGPT)
    
    // Step 3: Analyze text
    if (transcribed_text.length() > 0) {
      String analysis_result = analyze_text_with_chatgpt(transcribed_text);
      
      if (analysis_result.length() > 0) {
        String analysis_label = extractAnalysisLabel(analysis_result);
        printAnalysisResultPretty(analysis_result);
        if (analysis_label == "yes") {
          
          Serial.println(">>> ALERT: Bullying behavior detected!");
          bot.sendMessage(chatID, "ALERT: Bullying behavior detected in the latest transcription:\n\n\"" + transcribed_text + "\"\n\nAnalysis: " + analysis_result);
          
          String sensorData = buildSensorPayload(transcribed_text, analysis_result);
          bool sensorPublished = publishViaAPI(topic_subs, sensorData, 1);    //mqtt publish to EMQX Serverless API

          // Reconnect ACK subscriber immediately so app acknowledgments are less likely to be missed.
          resumeMqttAck();
          if (!waitForMqttAckReady(1500)) {
            Serial.println(">>> WARNING: ACK listener not ready yet");
          }

          if (sensorPublished) {
            Serial.println(">>> Sensor data published successfully.");
          } else {
            Serial.println(">>> Failed to publish sensor data.");
          }

          blinkLed13(5, 500);

        } else {
          Serial.println(">>> No bullying behavior detected.");
        }
      }
    } else {
      Serial.println(">>> Skipping ChatGPT analysis - No transcription available");
    }

    resumeMqttAck();
    digitalWrite(ledBlink, LOW); 
    
    pbcond = 1;
  }
  if (pushNoti == LOW) {
    pbcond = 0;
  }
  delay(50);
  
}