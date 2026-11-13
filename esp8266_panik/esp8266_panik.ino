#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiClient.h>
#include <WiFiClientSecureBearSSL.h>
#include "secrets.h"

// ====== CHANGE THESE ======
// Wi-Fi name, password and server address are in secrets.h (same folder, not uploaded to GitHub).
const char* DEVICE_ID   = "KDA-123X";
float baseLat = -1.9441;   // test location (Kigali), used while there is no GPS
float baseLng = 30.0619;
// ==========================

// ====== HARDWARE SWITCHES (1 = wired, 0 = not wired) ======
#define USE_KIT_BUTTON  1   // kit PUSH BUTTON module: D -> D7, VCC -> 3V3, GND -> G
#define USE_LED_MODULE  1   // kit LED module: LED3 (green) -> D1, LED2 (yellow) -> D2, LED1 (red) -> D0, GND -> G
// While USE_LED_MODULE is 0 the LEDs are VIRTUAL: shown on the phone page, in the Serial Monitor,
// and as flashes of the board's own blue LED (1, 2 or 3 flashes = level; slow blink = cancel requested).
// ===========================================================

// Buttons: tap = press (2-3 quick taps raise the level), hold 5 s = cancel request
#define FLASH_PIN   0    // the NodeMCU's own FLASH button (no wires). Do not hold it while the board starts.
#define KIT_BTN_PIN 13   // D7
#define CANCEL_PIN  14   // D5: jumper wire from D5 to GND held 5 s (optional)
#define LED_PIN     2    // built-in blue LED (D4), ON when pin is LOW

struct TapButton {
  int pin; const char* name; int idle;
  bool raw, down, cancelDone;
  unsigned long change, downAt;
};
TapButton flashBtn = {FLASH_PIN, "FLASH button", HIGH, false, false, false, 0, 0};
TapButton kitBtn   = {KIT_BTN_PIN, "kit button", LOW, false, false, false, 0, 0};

// 3 level LEDs (test only; a real hidden device would have no lights)
const int LEVEL_LEDS[3] = {5, 4, 16};   // D1 = level 1 (green), D2 = level 2 (yellow), D0 = level 3 (red)

const unsigned long LEVEL_WINDOW_MS = 3000;   // presses within 3 s of the first one raise the level
const unsigned long SEND_EVERY_MS   = 3000;   // location update while an incident is open
const unsigned long HEARTBEAT_MS    = 30000;  // "device online" check while idle

ESP8266WebServer web(80);       // the remote-button page: open http://<board IP>/ on a phone or laptop

int  level = 0;                 // current incident level (only goes up)
int  burstCount = 0;            // presses in the current 3-second window
unsigned long burstStart = 0;
bool active = false, cancelReq = false, sendNow = false;
int  incidentId = 0;
int  sendCounter = 0;
int  lastCode = 0;              // last HTTP code from the Panik server (shown on the remote page)
unsigned long lastSend = 0, cancelStart = 0, lastHeartbeat = 0;

void blinkLed(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, LOW); delay(80);
    digitalWrite(LED_PIN, HIGH); delay(80);
  }
}

// Where the device is, and where that position came from: "gps" or "test".
// When a GPS module is added, fill lat/lng from it here and return "gps" when it has a fix.
const char* getLocation(float &lat, float &lng) {
  sendCounter++;
  lat = baseLat + 0.00003 * sendCounter;   // simulated walking
  lng = baseLng + 0.00002 * sendCounter;
  return "test";
}

// POST a JSON body to SERVER_BASE + path. Returns the HTTP code and fills resp on 200.
int postJson(const char* path, const String& body, String& resp) {
  String url = String(SERVER_BASE) + path;
  WiFiClient plain;
  BearSSL::WiFiClientSecure secure;
  secure.setInsecure();               // prototype only: encrypts but does not check the certificate
  secure.setBufferSizes(1024, 512);   // keeps memory use low on the ESP8266
  HTTPClient http;
  http.setTimeout(8000);
  if (url.startsWith("https://")) http.begin(secure, url);
  else http.begin(plain, url);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (code == 200) resp = http.getString();
  http.end();
  return code;
}

// ---------------------------------------------------------------- buttons
void setupButtons() {
  pinMode(FLASH_PIN, INPUT_PULLUP);           // FLASH: not pressed = HIGH, pressed = LOW
  pinMode(CANCEL_PIN, INPUT_PULLUP);
#if USE_KIT_BUTTON
  pinMode(KIT_BTN_PIN, INPUT);                // the kit module has its own resistor
  delay(30);
  kitBtn.idle = digitalRead(KIT_BTN_PIN);     // learn which level means "not pressed"
  Serial.printf("\nKit button on D7: idle %s, pressed %s\n",
                kitBtn.idle == HIGH ? "HIGH" : "LOW", kitBtn.idle == HIGH ? "LOW" : "HIGH");
#endif
}

// tap (< 1.5 s) = press; hold 5 s = cancel request
void handleButton(TapButton &b, unsigned long now) {
  bool pressed = digitalRead(b.pin) != b.idle;
  if (pressed != b.raw) { b.raw = pressed; b.change = now; }   // debounce: wait until stable
  if (now - b.change < 40) return;

  if (pressed && !b.down) {                                     // went down
    b.down = true; b.downAt = now; b.cancelDone = false;
  } else if (pressed && b.down && !b.cancelDone && now - b.downAt >= 5000) {   // held 5 s
    b.cancelDone = true;
    Serial.println(requestCancel(b.name));
  } else if (!pressed && b.down) {                              // released
    b.down = false;
    if (!b.cancelDone && now - b.downAt < 1500) registerPress(b.name);
  }
}

// ---------------------------------------------------------------- level LEDs (real or virtual)
void setupLevelLeds() {
#if USE_LED_MODULE
  for (int i = 0; i < 3; i++) { pinMode(LEVEL_LEDS[i], OUTPUT); digitalWrite(LEVEL_LEDS[i], LOW); }
  for (int i = 0; i < 3; i++) { digitalWrite(LEVEL_LEDS[i], HIGH); delay(200); }   // start-up check 1, 2, 3
  delay(300);
  for (int i = 0; i < 3; i++) digitalWrite(LEVEL_LEDS[i], LOW);
#endif
}

// Level 1 = 1 LED, level 2 = 2 LEDs, level 3 = all 3. Blinking = cancel requested, waiting for police.
void showLevel(unsigned long now) {
  bool on = !cancelReq || (now / 500) % 2;
#if USE_LED_MODULE
  for (int i = 0; i < 3; i++)
    digitalWrite(LEVEL_LEDS[i], (active && on && level > i) ? HIGH : LOW);
#else
  // virtual LEDs on the board's blue LED: 1/2/3 short flashes every 2 s = level, slow blink = cancel
  bool blue = false;
  if (active) {
    if (cancelReq) blue = on;
    else { int slot = (now % 2000) / 200; blue = slot < level * 2 && slot % 2 == 0; }
  }
  digitalWrite(LED_PIN, blue ? LOW : HIGH);
#endif
  // virtual LEDs in the Serial Monitor (printed only when they change)
  static String lastLeds = "";
  String leds = String(active && level >= 1 ? "[G]" : "[ ]") + (active && level >= 2 ? "[Y]" : "[ ]") +
                (active && level >= 3 ? "[R]" : "[ ]") + (active && cancelReq ? " blinking: cancel requested" : "");
  if (leds != lastLeds) { lastLeds = leds; Serial.println("LEDs " + leds); }
}

// ---------------------------------------------------------------- press / cancel (all inputs use these)
// 1 press = level 1, 2 within 3 s = level 2, 3+ within 3 s = level 3. Level never goes down.
void registerPress(const char* from) {
  unsigned long now = millis();
  if (burstCount == 0 || now - burstStart > LEVEL_WINDOW_MS) { burstStart = now; burstCount = 1; }
  else burstCount++;
  int burstLevel = burstCount > 3 ? 3 : burstCount;
  if (burstLevel > level) level = burstLevel;
  active = true;
  sendNow = true;
  Serial.printf("PRESS (%s, burst %d) -> level %d\n", from, burstCount, level);
}

// returns a short message for whoever asked
const char* requestCancel(const char* from) {
  if (!active) return "No active alert to cancel";
  if (cancelReq) return "Cancel already requested";
  cancelReq = true;
  sendNow = true;
  Serial.printf("CANCEL requested (%s)\n", from);
  return "Cancel requested. Police will verify";
}

// ---------------------------------------------------------------- remote-button web page
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><meta name="theme-color" content="#0b1736">
<title>Panik button</title><style>
body{margin:0;font-family:system-ui,sans-serif;background:#f1f5f9;color:#0f172a;text-align:center}
header{background:#0b1736;color:#fff;padding:14px;font-weight:800;letter-spacing:.08em}
main{max-width:460px;margin:0 auto;padding:18px}
.b{width:100%;border:0;border-radius:22px;color:#fff;font-size:30px;font-weight:800;padding:46px 10px;margin:10px 0;
 user-select:none;-webkit-user-select:none;touch-action:manipulation;-webkit-tap-highlight-color:transparent}
#p{background:#e5383b;box-shadow:0 8px 0 #a4161a}#p:active{transform:translateY(6px);box-shadow:0 2px 0 #a4161a}
#c{background:#475569;font-size:22px;padding:28px 10px;position:relative;overflow:hidden}
#bar{position:absolute;left:0;bottom:0;height:8px;width:0;background:#a78bfa}
#s{font-size:20px;font-weight:700;margin-top:14px;padding:14px;border-radius:14px;background:#fff}
.leds{display:flex;justify-content:center;gap:22px;margin:16px 0 4px}
.leds i{width:38px;height:38px;border-radius:50%;background:#cbd5e1;box-shadow:inset 0 -4px 0 rgba(0,0,0,.15)}
.leds i.on1{background:#22c55e;box-shadow:0 0 16px #22c55e}.leds i.on2{background:#facc15;box-shadow:0 0 16px #facc15}
.leds i.on3{background:#ef4444;box-shadow:0 0 16px #ef4444}.leds i.blink{animation:bl 1s infinite}
@keyframes bl{50%{opacity:.2}}
.l1{color:#a16207}.l2{color:#c2410c}.l3{color:#b91c1c}.ok{color:#15803d}
small{color:#64748b}</style></head><body>
<header>PANIK &middot; <span id="dev"></span></header><main>
<button class="b" id="p">PRESS</button>
<button class="b" id="c">CANCEL<br><small style="color:#cbd5e1">hold 5 seconds</small><span id="bar"></span></button>
<div class="leds"><i id="L1"></i><i id="L2"></i><i id="L3"></i></div>
<small>level lights</small>
<div id="s">Connecting...</div>
<p><small>1 tap = Level 1 &middot; 2 taps within 3 s = Level 2 &middot; 3+ taps = Level 3</small></p></main>
<script>
const s=document.getElementById('s'),bar=document.getElementById('bar');
function show(d){document.getElementById('dev').textContent=d.device;
 for(let k=1;k<=3;k++)document.getElementById('L'+k).className=d.active&&d.level>=k?'on'+k+(d.cancel?' blink':''):'';
 if(!d.active){s.className='ok';s.textContent='Standby - ready'+(d.code&&d.code!=200?' (server: '+d.code+')':'');return;}
 s.className='l'+d.level;
 s.textContent='ALERT ACTIVE - Level '+d.level+(d.cancel?' - cancel requested':'')+(d.code&&d.code!=200?' (server error '+d.code+')':'');}
async function call(path,m){try{const r=await fetch(path,{method:m||'GET'});const d=await r.json();show(d);if(d.msg)s.textContent=d.msg;}
 catch(e){s.className='l3';s.textContent='Cannot reach the Panik device';}}
document.getElementById('p').addEventListener('click',()=>{if(navigator.vibrate)navigator.vibrate(60);call('/press','POST');});
const c=document.getElementById('c');let t=null,st=0,anim=null;
function stop(){clearTimeout(t);cancelAnimationFrame(anim);bar.style.width=0;}
c.addEventListener('pointerdown',e=>{e.preventDefault();st=Date.now();
 (function f(){bar.style.width=Math.min(100,(Date.now()-st)/50)+'%';anim=requestAnimationFrame(f)})();
 t=setTimeout(()=>{stop();if(navigator.vibrate)navigator.vibrate([80,60,80]);call('/cancel','POST');},5000);});
['pointerup','pointerleave','pointercancel'].forEach(ev=>c.addEventListener(ev,stop));
c.addEventListener('contextmenu',e=>e.preventDefault());
call('/status');setInterval(()=>call('/status'),2000);
</script></body></html>)HTML";

String statusJson(const char* msg = nullptr) {
  String j = "{\"device\":\"" + String(DEVICE_ID) + "\",\"active\":" + (active ? "true" : "false") +
             ",\"level\":" + String(level) + ",\"cancel\":" + (cancelReq ? "true" : "false") +
             ",\"incident\":" + String(incidentId) + ",\"code\":" + String(lastCode);
  if (msg) j += ",\"msg\":\"" + String(msg) + "\"";
  return j + "}";
}

void setupWeb() {
  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html", PAGE); });
  web.on("/status", HTTP_GET, []() { web.send(200, "application/json", statusJson()); });
  web.on("/press", HTTP_POST, []() {
    registerPress("remote");
    web.send(200, "application/json", statusJson());
  });
  web.on("/cancel", HTTP_POST, []() {
    const char* msg = requestCancel("remote");
    web.send(200, "application/json", statusJson(msg));
  });
  web.onNotFound([]() { web.send(404, "text/plain", "Not found"); });
  web.begin();
  if (MDNS.begin("panik")) MDNS.addService("http", "tcp", 80);
}

// ---------------------------------------------------------------- talking to the Panik server
// Explains an HTTP -1 (no connection) once, so the Serial Monitor says what to check.
void explainCode(int code) {
  static bool told = false;
  if (code > 0) { told = false; return; }
  if (told) return;
  told = true;
  Serial.printf("\n!! Cannot reach the server at %s\n", SERVER_BASE);
  Serial.print("   This board is ");
  Serial.print(WiFi.localIP());
  Serial.printf(" on \"%s\". Check that:\n", WIFI_SSID);
  Serial.println("   1. the laptop is connected to the SAME Wi-Fi");
  Serial.println("   2. SERVER_BASE has the laptop's CURRENT IP (run `ip a` on the laptop)");
  Serial.println("   3. python server.py is running on the laptop\n");
}

void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return;
  String body = "{\"device_id\":\"" + String(DEVICE_ID) + "\",\"rssi\":" + String(WiFi.RSSI()) + "}";
  String resp;
  lastCode = postJson("/api/heartbeat", body, resp);
  Serial.printf("Heartbeat -> HTTP %d\n", lastCode);
  explainCode(lastCode);
}

void sendAlert() {
  if (WiFi.status() != WL_CONNECTED) { Serial.println("No Wi-Fi"); return; }
  float lat, lng;
  const char* src = getLocation(lat, lng);

  String body = "{\"device_id\":\"" + String(DEVICE_ID) + "\",\"incident_id\":" + String(incidentId) +
                ",\"level\":" + String(level) + ",\"cancel\":" + (cancelReq ? "true" : "false") +
                ",\"lat\":" + String(lat, 6) + ",\"lng\":" + String(lng, 6) +
                ",\"src\":\"" + src + "\"}";

  String resp;
  int code = postJson("/api/alert", body, resp);
  lastCode = code;
  Serial.printf("Sent level %d (%s) -> HTTP %d\n", level, src, code);
  explainCode(code);
  if (code != 200) return;

  resp.replace(" ", "");                       // e.g. {"closed":false,"incident_id":3}
  int idx = resp.indexOf("\"incident_id\":");
  if (idx >= 0) incidentId = resp.substring(idx + 14).toInt();

  if (resp.indexOf("\"closed\":true") >= 0) {  // police closed the incident
    Serial.println("Incident closed by police. Back to standby.");
    active = false; level = 0; burstCount = 0; cancelReq = false; incidentId = 0; sendCounter = 0;
    digitalWrite(LED_PIN, HIGH);
    lastHeartbeat = millis();
  }
}

// ---------------------------------------------------------------- Serial Monitor controls (optional)
void printHelp() {
  Serial.println("\n--- Controls ---");
  Serial.print("  Phone/laptop remote button:  http://");
  Serial.print(WiFi.localIP());
  Serial.println("/   (or http://panik.local/ on laptops)");
  Serial.println("  Buttons (FLASH" + String(USE_KIT_BUTTON ? " or kit button on D7" : "") + "): tap = press, hold 5 s = cancel");
  Serial.println("  Serial Monitor: p = press, c = cancel, h = help");
  Serial.println(USE_LED_MODULE ? "  LEDs: kit LED module on D1/D2/D0" : "  LEDs: virtual (phone page, Serial Monitor, blue LED flashes)");
  Serial.println("----------------\n");
}

void handleSerial() {
  while (Serial.available()) {
    char k = Serial.read();
    if (k == 'p' || k == 'P') {
      if (level < 3) level++;             // in the Serial Monitor each p = one step more urgent
      active = true;
      sendNow = true;
      Serial.printf("PRESS (serial) -> level %d%s\n", level, level == 3 ? " (max)" : "");
    } else if (k == 'c' || k == 'C') {
      Serial.println(requestCancel("serial"));
    } else if (k == 'h' || k == 'H' || k == '?') {
      printHelp();
    }
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  setupButtons();
  setupLevelLeds();

  Serial.printf("\nConnecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  for (int attempt = 1; ; attempt++) {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(500); Serial.print("."); }
    IPAddress ip = WiFi.localIP();
    if (WiFi.status() == WL_CONNECTED && ip[0] != 169 && ip[0] != 0) break;   // got a real address
    Serial.printf("\nNo proper IP yet (got %s), retrying (%d)...\n", ip.toString().c_str(), attempt);
    WiFi.disconnect();
    delay(1000);
  }
  Serial.print("\nConnected. IP: ");
  Serial.println(WiFi.localIP());
  Serial.printf("Server: %s\n", SERVER_BASE);
  setupWeb();
  blinkLed(3);
  printHelp();
  lastHeartbeat = millis() - HEARTBEAT_MS;   // send the first heartbeat straight away
}

void loop() {
  unsigned long now = millis();
  web.handleClient();     // remote button page
  MDNS.update();

  // buttons: tap = press, hold 5 s = cancel
  handleButton(flashBtn, now);
#if USE_KIT_BUTTON
  handleButton(kitBtn, now);
#endif
  handleSerial();

  // physical CANCEL: hold D5 to GND for 5 seconds
  if (digitalRead(CANCEL_PIN) == LOW) {
    if (cancelStart == 0) cancelStart = now;
    else if (now - cancelStart >= 5000 && active && !cancelReq) requestCancel("button");
  } else {
    cancelStart = 0;
  }

  // send immediately on a press, then every 3 seconds while active
  if (active && (sendNow || now - lastSend >= SEND_EVERY_MS)) {
    sendNow = false;
    lastSend = now;
    sendAlert();
  }

  // idle: small "device online" check every 30 seconds
  if (!active && now - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = now;
    sendHeartbeat();
  }

  showLevel(now);
}