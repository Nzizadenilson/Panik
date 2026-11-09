#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

// ====== CHANGE THESE ======
const char* WIFI_SSID  = "YOUR_WIFI_NAME";
const char* WIFI_PASS  = "YOUR_WIFI_PASSWORD";
const char* SERVER_URL = "http://YOUR_LAPTOP_IP:5000/api/alert";  // your laptop IP
const char* DEVICE_ID  = "KDA-123X";
float baseLat = -1.2921;   // test location
float baseLng = 36.8219;
// ==========================

// Buttons: a wire between the pin and GND = pressed. NEVER connect GND to 3V.
#define PRESS_PIN  12   // D6
#define CANCEL_PIN 14   // D5
#define LED_PIN    2    // built-in LED (D4), ON when pin is LOW

int  pressCount = 0;
bool active = false, cancelReq = false, sendNow = false;
int  incidentId = 0;
int  sendCounter = 0;
bool lastPress = HIGH;
unsigned long lastPressTime = 0, lastSend = 0, cancelStart = 0;

void blinkLed(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LOW); delay(80);
    digitalWrite(LED_PIN, HIGH); delay(80);
  }
}

void registerPress() {
  pressCount++;
  active = true;
  sendNow = true;
  Serial.printf("PRESS #%d\n", pressCount);
}

void sendAlert() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("No Wi-Fi"); return; }
  sendCounter++;
  float lat = baseLat + 0.00003 * sendCounter;   // simulated walking
  float lng = baseLng + 0.00002 * sendCounter;
  int level = pressCount > 3 ? 3 : pressCount;

  String body = "{\"device_id\":\"" + String(DEVICE_ID) + "\",\"incident_id\":" + String(incidentId) +
                ",\"level\":" + String(level) + ",\"cancel\":" + (cancelReq ? "true" : "false") +
                ",\"lat\":" + String(lat, 6) + ",\"lng\":" + String(lng, 6) + "}";

  WiFiClient client;
  HTTPClient http;
  http.setTimeout(4000);
  http.begin(client, SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);

  if (code > 0) Serial.printf("Sent level %d -> HTTP %d\n", level, code);
  else Serial.printf("Send FAILED: %s (check SERVER_URL, server running, same Wi-Fi)\n",
                     http.errorToString(code).c_str());

  if (code == 200) {
    String resp = http.getString();
    int idx = resp.indexOf("incident_id");
    if (idx >= 0) {
      idx = resp.indexOf(':', idx);
      incidentId = resp.substring(idx + 1).toInt();
    }
    if (resp.indexOf("true") >= 0) {
      Serial.println("Incident closed by police. Back to standby.");
      active = false; pressCount = 0; cancelReq = false; incidentId = 0; sendCounter = 0;
      digitalWrite(LED_PIN, HIGH);
    } else {
      blinkLed(1);
    }
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  pinMode(PRESS_PIN, INPUT_PULLUP);
  pinMode(CANCEL_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial.printf("\nConnecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.print("\nConnected. IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("Ready. Type p + Enter in this monitor to simulate PRESS, c + Enter for CANCEL.");
  blinkLed(3);
}

void loop() {
  // Test without any button: type 'p' or 'c' in the Serial Monitor
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == 'p') registerPress();
    if (ch == 'c' && active) { cancelReq = true; sendNow = true; Serial.println("CANCEL requested"); }
  }

  // PRESS: wire between D6 and GND
  bool p = digitalRead(PRESS_PIN);
  if (p == LOW && lastPress == HIGH && millis() - lastPressTime > 250) {
    lastPressTime = millis();
    registerPress();
  }
  lastPress = p;

  // CANCEL: wire between D5 and GND, held for 5 seconds
  if (digitalRead(CANCEL_PIN) == LOW) {
    if (cancelStart == 0) cancelStart = millis();
    else if (millis() - cancelStart >= 5000 && active && !cancelReq) {
      cancelReq = true;
      sendNow = true;
      Serial.println("CANCEL requested");
    }
  } else {
    cancelStart = 0;
  }

  if (active && (sendNow || millis() - lastSend >= 3000)) {
    sendNow = false;
    lastSend = millis();
    sendAlert();
  }
}