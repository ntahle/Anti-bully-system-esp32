#ifndef HANDLERS_H
#define HANDLERS_H

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include "chatgpt_handler.h"

extern WebServer server;
extern int led;
extern bool wifi_connected;


// EEPROM addresses
#define EEPROM_SSID_ADDR 0
#define EEPROM_PASS_ADDR 32
#define EEPROM_SIZE 128

String getConnectionStatus() {
  String status = "";
  
  if (WiFi.status() == WL_CONNECTED) {
    status += "<p style='color: green; font-weight: bold;'>Connected to WiFi</p>";
    status += "<p>SSID: " + WiFi.SSID() + "</p>";
    status += "<p>IP Address: " + WiFi.localIP().toString() + "</p>";
  } else {
    status += "<p style='color: red; font-weight: bold;'>Not Connected</p>";
    status += "<p>Status Code: " + String(WiFi.status()) + "</p>";
  }
  
  return status;
}

void handleRoot() {
  String html = R"(
    <!DOCTYPE html>
    <html>
    <head>
      <title>WiFi Config</title>
      <style>
        body { font-family: Arial; text-align: center; padding: 20px; }
        .status { border: 2px solid #ddd; padding: 15px; margin: 20px 0; border-radius: 5px; }
        input { padding: 8px; margin: 5px; width: 200px; }
        button { padding: 10px 20px; margin: 5px; border: none; cursor: pointer; border-radius: 4px; font-weight: bold; }
        .save-btn { background: #4CAF50; color: white; }
        .save-btn:hover { background: #45a049; }
        .reset-btn { background: #f44336; color: white; }
        .reset-btn:hover { background: #da190b; }
        .button-group { margin: 15px 0; }
      </style>
    </head>
    <body>
      <h1>WiFi Configuration</h1>
      
      <div class="status">
        <h2>Connection Status</h2>
        )" + getConnectionStatus() + R"(
      </div>
      
      <h2>Configure WiFi</h2>
      <form action="/save" method="POST">
        <input type="text" name="ssid" placeholder="WiFi SSID" required><br>
        <input type="password" name="pass" placeholder="WiFi Password" required><br>
        <div class="button-group">
          <button type="submit" class="save-btn">Save & Connect</button>
        </div>
      </form>
      
      
    </body>
    </html>
  )";
  server.send(200, "text/html", html);
}

void connectToWiFi() {
  EEPROM.begin(EEPROM_SIZE);
  char ssid[32] = {0};
  char pass[32] = {0};
  memcpy(ssid, (char*)EEPROM.getDataPtr() + EEPROM_SSID_ADDR, 32);
  memcpy(pass, (char*)EEPROM.getDataPtr() + EEPROM_PASS_ADDR, 32);
  EEPROM.end();
  
  Serial.print("Attempting to connect to: ");
  Serial.println(ssid);
  WiFi.begin(ssid, pass);
}

void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  
  // Save to EEPROM
  EEPROM.begin(EEPROM_SIZE);
  ssid.toCharArray((char*)EEPROM.getDataPtr() + EEPROM_SSID_ADDR, 32);
  pass.toCharArray((char*)EEPROM.getDataPtr() + EEPROM_PASS_ADDR, 32);
  EEPROM.commit();
  EEPROM.end();
  
  server.send(200, "text/html", "<h1>Saved! Connecting to WiFi...</h1>");
  delay(2000);
  connectToWiFi();
}

void handleReset() {
  Serial.println("Resetting WiFi credentials...");
  
  
  // Clear EEPROM
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  EEPROM.end();
  
  // Disconnect from WiFi
  WiFi.disconnect();
  
  String html = "<h1>Reset Complete!</h1>";
  html += "<p>All credentials have been cleared.</p>";
  html += "<p>Please reconnect to ESP32_Config and enter new credentials.</p>";
  html += "<p>Redirecting in 3 seconds...</p>";
  html += "<script>setTimeout(function() { window.location.href = '/'; }, 3000);</script>";
  
  server.send(200, "text/html", html);
  
  Serial.println("EEPROM cleared. Ready for new credentials.");
}

void handleNotFound() {
  // Log the requested URL for debugging
  Serial.print("Request for: ");
  Serial.println(server.uri());
  
  // Redirect captive portal requests to root
  server.sendHeader("Location", "http://192.168.4.1/");
  server.send(302, "text/plain", "");
}

void handleCaptivePortal() {
  server.sendHeader("Location", "http://192.168.4.1/");
  server.send(302, "text/plain", "");
}

void Connected(){
  digitalWrite(led, HIGH);
  wifi_connected = true;
}

void Disconnected(){
  digitalWrite(led, LOW);
  delay(100);
  digitalWrite(led, HIGH);
  delay(100);
  wifi_connected = false;
}

#endif // HANDLERS_H
