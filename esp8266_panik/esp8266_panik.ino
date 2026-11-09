#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <WiFiClient.h>

// ====== CHANGE THESE ======
const char* WIFI_SSID   = "SOULHOSTELS.RW";
const char* WIFI_PASS   = "@fr1canLU";
const char* SERVER_BASE = "http://192.168.1.226:5000";   // laptop IP on the SAME Wi-Fi, no trailing slash
const char* DEVICE_ID   = "KDA-123X";
float baseLat = -1.9441;   // test location (Kigali), used while there is no GPS
float baseLng = 30.0619;
// ==========================

#define PRESS_PIN  0    // FLASH button on the NodeMCU (D3), if your board has one
#define CANCEL_PIN 14   // D5: touch a jumper wire from D5 to GND and hold 5 s
#define LED_PIN    2    // built-in LED (D4), ON when pin is LOW (test only; a real device has no light)

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
bool lastPress = HIGH;
unsigned long lastPressTime = 0, lastSend = 0, cancelStart = 0, lastHeartbeat = 0;

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
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(4000);
  http.begin(client, String(SERVER_BASE) + path);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (code == 200) resp = http.getString();
  http.end();
  return code;
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
.l1{color:#a16207}.l2{color:#c2410c}.l3{color:#b91c1c}.ok{color:#15803d}
small{color:#64748b}</style></head><body>
<header>PANIK &middot; <span id="dev"></span></header><main>
<button class="b" id="p">PRESS</button>
<button class="b" id="c">CANCEL<br><small style="color:#cbd5e1">hold 5 seconds</small><span id="bar"></span></button>
<div id="s">Connecting...</div>
<p><small>1 tap = Level 1 &middot; 2 taps within 3 s = Level 2 &middot; 3+ taps = Level 3</small></p></main>
<script>
const s=document.getElementById('s'),bar=document.getElementById('bar');
function show(d){document.getElementById('dev').textContent=d.device;
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
void sendHeartbeat() {
  if (WiFi.status() != WL_CONNECTED) return;
  String body = "{\"device_id\":\"" + String(DEVICE_ID) + "\",\"rssi\":" + String(WiFi.RSSI()) + "}";
  String resp;
  lastCode = postJson("/api/heartbeat", body, resp);
  Serial.printf("Heartbeat -> HTTP %d\n", lastCode);
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
  if (code != 200) return;

  resp.replace(" ", "");                       // e.g. {"closed":false,"incident_id":3}
  int idx = resp.indexOf("\"incident_id\":");
  if (idx >= 0) incidentId = resp.substring(idx + 14).toInt();

  if (resp.indexOf("\"closed\":true") >= 0) {  // police closed the incident
    Serial.println("Incident closed by police. Back to standby.");
    active = false; level = 0; burstCount = 0; cancelReq = false; incidentId = 0; sendCounter = 0;
    digitalWrite(LED_PIN, HIGH);
    lastHeartbeat = millis();
  } else {
    blinkLed(1);
  }
}

// ---------------------------------------------------------------- Serial Monitor controls (optional)
void printHelp() {
  Serial.println("\n--- Controls ---");
  Serial.print("  Phone/laptop remote button:  http://");
  Serial.print(WiFi.localIP());
  Serial.println("/   (or http://panik.local/ on laptops)");
  Serial.println("  Serial Monitor: p = press, c = cancel, h = help");
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
  setupWeb();
  blinkLed(3);
  printHelp();
  lastHeartbeat = millis() - HEARTBEAT_MS;   // send the first heartbeat straight away
}

void loop() {
  unsigned long now = millis();
  web.handleClient();     // remote button page
  MDNS.update();

  // physical PRESS button (if fitted)
  bool p = digitalRead(PRESS_PIN);
  if (p == LOW && lastPress == HIGH && now - lastPressTime > 250) {
    lastPressTime = now;
    registerPress("button");
  }
  lastPress = p;

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
}