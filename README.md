# Panik: discreet emergency button (prototype)

A driver presses a hidden button; the police console shows the alert, the vehicle's location and its
urgency level (1-3) at once. Repeated presses raise the level, holding the button for 5 s sends a
cancel request, and only the police can close an incident.

```
Panik/
  server.py                Flask API: alerts, incidents, history, share links, laptop location beacon
  requirements.txt         Python packages (Flask)
  .env.example             server settings; copy to .env (ignored by git)
  esp8266_panik/
    esp8266_panik.ino      NodeMCU / ESP8266 firmware
    secrets.example.h      template for secrets.h (plate, Wi-Fi networks, server address)
    secrets.h              your own copy - ignored by git, never commit it
  frontend/
    index.html             police console
    track.html             live location for officers on the ground (shared link)
    phone.html             phone backup panic button
    driver.html            laptop location beacon (the board has no GPS)
    css/style.css          shared styles (laptop + phone)
    js/                    common.js, police.js, track.js, phone.js, driver.js
    vendor/                optional offline copy of Leaflet (see Notes)
  panik.db                 SQLite database, created automatically - ignored by git
```

## 1. Server setup (first time)

```bash
cd Panik
python3 -m venv .venv                 # virtual environment (ignored by git)
source .venv/bin/activate             # Windows: .venv\Scripts\activate
pip install -r requirements.txt
cp .env.example .env                  # optional: change port, database file, timings
```

## 2. Device setup (first time)

Hardware (Menya IoT kit + NodeMCU ESP8266):

| Part | Connection |
|---|---|
| Push button module | D -> **D7**, VCC -> **3V3**, GND -> **G** |
| LED module (optional) | LED3 green -> **D1**, LED2 yellow -> **D2**, LED1 red -> **D0**, GND -> **G** |

Turn parts on or off with `USE_KIT_BUTTON` / `USE_LED_MODULE` at the top of the `.ino`.
The NodeMCU's own **FLASH** button always works as a backup button.

1. Arduino IDE -> Boards Manager: **esp8266**. Board: **NodeMCU 1.0 (ESP-12E Module)**.
2. Arduino IDE -> Library Manager: **ArduinoJson** by Benoit Blanchon, version **7.x**.
3. In `esp8266_panik/`, copy `secrets.example.h` to `secrets.h` and fill in:
   - `DEVICE_ID`: the vehicle plate shown to the police (each board its own),
   - `WIFI_NETWORKS`: one line per Wi-Fi the board may use, each with the **laptop's IP on that network**
     (`ip a` on the laptop while it is on that network), e.g. hostel Wi-Fi and a phone hotspot.
4. Upload `esp8266_panik/esp8266_panik.ino`. Serial Monitor at 115200 shows its IP and the server it uses.

## 3. Run

```bash
source .venv/bin/activate
python server.py
```

| Who | Open |
|---|---|
| Police (laptop) | `http://localhost:5000/` |
| Police (phone, same Wi-Fi) | `http://<laptop-ip>:5000/` |
| Laptop location beacon | `http://localhost:5000/driver` in **Firefox** (must be `localhost`) |
| Phone backup button | `http://<laptop-ip>:5000/phone` |
| Board's own button page | `http://<board-ip>/` (printed in the Serial Monitor) |

The database upgrades itself when new columns or tables are added.

## Using the board

| You do | Result |
|---|---|
| Tap | alert, level 1 |
| 2 / 3 taps within 3 s | level 2 / level 3 (the level never goes down) |
| Hold 5 s | cancel request: the police verify with the driver, then close |
| Police close the incident | board returns to standby |

LEDs: green / +yellow / +red = level 1 / 2 / 3; blinking = cancel requested.
Blue LED blinking fast = no Wi-Fi (presses are kept and sent once connected).

## Laptop location beacon

The board has no GPS, so its alerts use the laptop's position from the `/driver` page.

- **LIVE** - sending the laptop's location (Wi-Fi based, about 20-200 m accurate).
- **Set position on the map** - click where the vehicle is; exact, best for demos. Use this if the
  browser cannot locate (common in Brave/Chromium on Linux).
- If the page stops, alerts use the **last known** position (up to 6 h), labelled as such.
- "Used for vehicle" pairs the beacon with one plate; empty = any board.

## Using the police console

- **Tabs:** *Active*, *Dispatched*, *Cancels* (needs verifying), *History* (closed, with filters).
- **Accept & track** opens the map on that vehicle and follows it. **Dispatch unit**, **Close incident**.
- **Share live location:** secret link (WhatsApp, SMS, copy) for officers on the ground.
- **Map:** Streets / Satellite / Hybrid, road directions with drive time, route snapped to roads.
- **Sound on:** click once to allow alert sounds.
- Black flashing card = **LOST CONTACT**. Top bar: devices online/offline and the beacon status.

## Test without hardware

```bash
curl -X POST http://localhost:5000/api/alert -H "Content-Type: application/json" \
 -d '{"device_id":"TEST-1","incident_id":0,"level":3,"lat":-1.9441,"lng":30.0619,"src":"test"}'
```
Bad input gets a clear `400` error (e.g. `{"error": "lat is required"}`). A single curl shows LOST CONTACT
after 15 s because nothing else is sent; use `/phone` for a continuous test alert.

## Contributing

- **Never commit** `secrets.h`, `.env` or `panik.db` (they are in `.gitignore`). Check `git status` first.
- One change per commit, with a message that says what and why, in the imperative:
  - `Add 3-second window for raising the alert level`
  - `Fix board freezing when the server is unreachable`
  - `Update README with secrets.h setup`
- Not: `updated`, `changes`, `Update panik.db with latest changes`.

## Notes

- **Share links** use the laptop's Wi-Fi address, so officers' phones must be on the same network.
  Hosting the server online (PythonAnywhere, Render...) makes links work anywhere and gives https,
  which lets phones use their real GPS.
- **Satellite/Hybrid** imagery is from Esri and routing from the public OSRM server (free, fair use, no key).
- **The map needs internet.** If unpkg.com is blocked, put `leaflet.js` and `leaflet.css` from
  https://unpkg.com/leaflet@1.9.4/dist/ in `frontend/vendor/`; the pages fall back to them.
  Alerts work even when the map cannot load.
- **Prototype limits:** no police login yet, location is from the laptop (no GPS module), and the board
  accepts the server's https certificate without checking it.