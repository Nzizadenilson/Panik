# Panik: discreet emergency button (prototype)

```
Panik/
  server.py              Flask API only (alerts, incidents, history, share links)
  panik_esp8266.ino      NodeMCU / ESP8266 button firmware (unchanged API)
  panik.db               created automatically (SQLite)
  frontend/
    index.html           Police console
    track.html           Live-location page for officers on the ground (shared link)
    phone.html           Phone backup panic button
    driver.html          Laptop acting as the vehicle GPS
    css/style.css        Shared styles (responsive: laptop + phone)
    js/common.js         Shared helpers
    js/police.js         Police console logic
    js/track.js  js/phone.js  js/driver.js
    vendor/              (optional) offline copy of Leaflet, see below
```

## Run

```bash
pip install flask
python server.py
```

The terminal prints the addresses. Then:

| Who | Open |
|---|---|
| Police (laptop) | `http://localhost:5000/` |
| Police (phone, same Wi-Fi) | `http://<laptop-ip>:5000/` |
| Laptop as vehicle GPS | `http://localhost:5000/driver` (must be `localhost`) |
| Phone backup button | `http://<laptop-ip>:5000/phone` |

Your old `panik.db` keeps working: the new columns are added automatically.

## Using the police console

- **Tabs:** *Active* (new + accepted), *Dispatched*, *Cancels* (driver held CANCEL, needs verifying),
  *History* (closed). Several incidents at once each sit in their own tab, newest/most urgent first.
- **Accept & track:** accepts the alert and opens the map focused on that vehicle, following it live.
- **Dispatch unit:** moves the incident to *Dispatched*.
- **Share live location:** creates a secret link (WhatsApp, SMS, copy) that officers on the ground open
  on their phone to follow the vehicle and get directions. The link stops updating when the incident is closed.
- **Close incident:** *resolved*, or for cancel requests *driver verified safe - close as cancelled*.
  Closed incidents go to *History*, which shows whether each one was accepted, dispatched or cancelled
  (filter chips at the top), with the full timeline and route.
- **Map views:** the switcher at the top-right of the map changes between *Streets*, *Satellite* and
  *Hybrid* (satellite + road names). The choice is remembered.
- **Directions:** shows the road route, distance and drive time from the unit to the vehicle, and updates
  it as the vehicle moves. The start is your location when the browser allows it; otherwise tap the map
  (e.g. the police station). *Google Maps* opens turn-by-turn navigation.
- **Route on roads:** the vehicle's path is snapped to real roads (OpenStreetMap map matching), so it
  no longer cuts through buildings. The faint circle around the vehicle is the location accuracy (e.g. ±60 m).
- **Sound on:** click once to allow alert sounds (browsers block sound until you click).
- Black flashing card = **LOST CONTACT** (no update for 15 s). Device chips at the top show online/offline.

## Test without hardware

```bash
curl -X POST http://localhost:5000/api/alert -H "Content-Type: application/json" \
 -d '{"device_id":"TEST-1","incident_id":0,"level":3,"lat":-1.9441,"lng":30.0619,"src":"test"}'
```
A single curl shows LOST CONTACT after 15 s because nothing else is sent; that is expected.
Use the phone page (`/phone`) for a continuous test alert.

## Location accuracy

Laptop (Wi-Fi) location is only accurate to about 20-200 m and wanders even when you stand still.
The server holds the last position until a new reading moves further than its accuracy (min 15 m,
max 100 m), and skips readings much worse than a recent good one. For real accuracy (3-10 m) use a
GPS module (NEO-6M) or the phone's GPS over https.

## Notes

- **Share links** use the laptop's Wi-Fi address, so officers' phones must be on the same network.
  To share with anyone, host the server online (Render, PythonAnywhere...) - that also gives https,
  which lets the phone page use the phone's real GPS.
- **Satellite/Hybrid** imagery is from Esri and routing from the public OSRM server (free, fair use, no key).
- **Map needs internet** (Leaflet from unpkg.com and OpenStreetMap tiles). If unpkg is blocked, download
  `leaflet.js` and `leaflet.css` from https://unpkg.com/leaflet@1.9.4/dist/ into `frontend/vendor/`;
  the pages fall back to that copy automatically. Alerts work even when the map cannot load.
