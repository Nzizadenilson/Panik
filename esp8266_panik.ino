#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

// ====== CHANGE THESE ======
const char* WIFI_SSID  = "CANALBOX-7B44-2G";
const char* WIFI_PASS  = "LA5EyrGzaG7n";
const char* SERVER_URL = "http://192.168.43.100:5000/api/alert";  // laptop IP on the SAME hotspot
const char* DEVICE_ID  = "KDA-123X";
float baseLat = -1.2921;   // test location (Nairobi). Change to where you are if you like
float baseLng = 36.8219;
// ==========================

#define PRESS_PIN  0    // FLASH button on the NodeMCU (D3)
#define CANCEL_PIN 14   // D5: touch a jumper wire from D5 to GND and hold 5 s
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
  blinkLed(3);
}

void sendAlert() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("No Wi-Fi"); return; }
  sendCounter++;
  // simulated walking: the point drifts a little with every send
  float lat = baseLat + 0.00003 * sendCounter;
  float lng = baseLng + 0.00002 * sendCounter;
  int level = pressCount > 3 ? 3 : pressCount;

  String body = "{\"device_id\":\"" + String(DEVICE_ID) + "\",\"incident_id\":" + String(incidentId) +
                ",\"level\":" + String(level) + ",\"cancel\":" + (cancelReq ? "true" : "false") +
                ",\"lat\":" + String(lat, 6) + ",\"lng\":" + String(lng, 6) + "}";

  WiFiClient client;
  HTTPClient http;
  http.begin(client, SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  Serial.printf("Sent level %d -> HTTP %d\n", level, code);

  if (code == 200) {
    String resp = http.getString();   // e.g. {"closed":false,"incident_id":3}
    int idx = resp.indexOf("incident_id");
    if (idx >= 0) {
      idx = resp.indexOf(':', idx);
      incidentId = resp.substring(idx + 1).toInt();
    }
    if (resp.indexOf("true") >= 0) {  // police closed the incident
      Serial.println("Incident closed by police. Back to standby.");
      active = false; pressCount = 0; cancelReq = false; incidentId = 0; sendCounter = 0;
      digitalWrite(LED_PIN, HIGH);
    } else {
      blinkLed(1);
    }
  }
  http.end();
}

void loop() {
  // PRESS button (detect each new press)
  bool p = digitalRead(PRESS_PIN);
  if (p == LOW && lastPress == HIGH && millis() - lastPressTime > 250) {
    lastPressTime = millis();
    pressCount++;
    active = true;
    sendNow = true;
    Serial.printf("PRESS #%d\n", pressCount);
  }
  lastPress = p;

  // CANCEL: hold 5 seconds
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

  // send immediately on a press, then every 3 seconds while active
  if (active && (sendNow || millis() - lastSend >= 3000)) {
    sendNow = false;
    lastSend = millis();
    sendAlert();
  }
}