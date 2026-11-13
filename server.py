"""Panik server: Flask API only. The web pages live in ./frontend."""
import math
import os
import secrets
import socket
import sqlite3
import time

from flask import Flask, request, jsonify

BASE = os.path.dirname(os.path.abspath(__file__))


def load_env(path):
    """Read KEY=value lines from .env into the environment (real environment variables win)."""
    if not os.path.exists(path):
        return
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


def env_int(name, default):
    try:
        return int(os.environ.get(name, default))
    except ValueError:
        print(f"  ! {name} in .env is not a whole number - using {default}")
        return default


load_env(os.path.join(BASE, ".env"))   # settings: see .env.example

HOST = os.environ.get("PANIK_HOST", "0.0.0.0")
PORT = env_int("PANIK_PORT", 5000)
DB = os.path.join(BASE, os.environ.get("PANIK_DB", "panik.db"))   # relative paths are next to server.py
app = Flask(__name__, static_folder=os.path.join(BASE, "frontend"), static_url_path="")

# Laptop location beacon: the /driver page on the laptop sends its position, and alerts from the
# board use it (the board has no GPS). Kept in the database, so a server restart does not lose it.
BEACON_LIVE_S = 30        # a beacon fix newer than this is "live"
BEACON_LAST_KNOWN_S = 6 * 3600   # older fixes are still used as "last known" for up to 6 hours
BEACON_MAX_FIX_AGE_S = 60        # the browser's own fix must be newer than this to be accepted
ONLINE_TIMEOUT = env_int("PANIK_ONLINE_TIMEOUT_S", 75)      # s without contact -> device offline
REUSE_WINDOW_S = env_int("PANIK_REUSE_WINDOW_S", 300)       # a restarted device continues its open incident
                                                            # only if it was updated this recently
LOST_CONTACT_AFTER = env_int("PANIK_LOST_CONTACT_S", 15)    # s without a location update -> lost contact
# Location cleaning: Wi-Fi/laptop positions jump around by tens of metres even when standing still.
MIN_MOVE_M = 15           # a new point must be at least this far from the last one to count as movement
MAX_JITTER_M = 100        # ...or further than its own accuracy, capped at this
BAD_ACC_M = 300           # readings worse than this are ignored when a recent better one exists

# columns added after the first version; added automatically to older databases
INCIDENT_COLS = {"acknowledged_at": "REAL", "dispatched_at": "REAL", "cancel_at": "REAL",
                 "closed_at": "REAL", "outcome": "TEXT", "share_token": "TEXT"}


# ---------------------------------------------------------------- database
def db():
    c = sqlite3.connect(DB, timeout=10)   # wait instead of failing if another request is writing
    c.row_factory = sqlite3.Row
    return c


def init():
    c = db()
    c.executescript("""
    CREATE TABLE IF NOT EXISTS incidents(
        id INTEGER PRIMARY KEY AUTOINCREMENT, device_id TEXT, level INTEGER,
        status TEXT, cancel_requested INTEGER DEFAULT 0, created REAL, updated REAL);
    CREATE TABLE IF NOT EXISTS locations(
        id INTEGER PRIMARY KEY AUTOINCREMENT, incident_id INTEGER,
        lat REAL, lng REAL, ts REAL);
    CREATE TABLE IF NOT EXISTS beacons(
        device TEXT PRIMARY KEY, lat REAL, lng REAL, acc REAL, manual INTEGER DEFAULT 0, ts REAL);
    CREATE TABLE IF NOT EXISTS devices(
        device_id TEXT PRIMARY KEY, ts REAL, rssi INTEGER, src TEXT);
    """)
    have = {r[1] for r in c.execute("PRAGMA table_info(incidents)")}
    for col, typ in INCIDENT_COLS.items():
        if col not in have:
            c.execute(f"ALTER TABLE incidents ADD COLUMN {col} {typ}")
    loc_cols = {r[1] for r in c.execute("PRAGMA table_info(locations)")}
    if "src" not in loc_cols:
        c.execute("ALTER TABLE locations ADD COLUMN src TEXT DEFAULT 'test'")
    if "acc" not in loc_cols:
        c.execute("ALTER TABLE locations ADD COLUMN acc REAL")
    c.execute("UPDATE incidents SET closed_at=updated WHERE status='closed' AND closed_at IS NULL")
    # indexes: the trail of every open incident is read every 2 s, so these lookups must stay fast
    c.executescript("""
    CREATE INDEX IF NOT EXISTS idx_locations_incident ON locations(incident_id, id);
    CREATE INDEX IF NOT EXISTS idx_incidents_device ON incidents(device_id, status);
    CREATE INDEX IF NOT EXISTS idx_incidents_status ON incidents(status, closed_at);
    CREATE INDEX IF NOT EXISTS idx_incidents_share ON incidents(share_token);
    """)
    c.execute("PRAGMA journal_mode=WAL")   # readers do not block the writer (police console polls a lot)
    c.commit()
    c.close()


def incident_dict(c, r, now, trail_limit=100):
    trail = c.execute("SELECT lat,lng,ts,src,acc FROM locations WHERE incident_id=? ORDER BY id DESC LIMIT ?",
                      (r["id"], max(trail_limit, 1))).fetchall()
    last = trail[0] if trail else None
    closed = r["status"] == "closed"
    last_ago = int(now - last["ts"]) if last else None
    return {
        "id": r["id"], "device_id": r["device_id"], "level": r["level"], "status": r["status"],
        "cancel_requested": bool(r["cancel_requested"]),
        "created": r["created"], "acknowledged_at": r["acknowledged_at"],
        "dispatched_at": r["dispatched_at"], "cancel_at": r["cancel_at"],
        "closed_at": r["closed_at"], "outcome": r["outcome"],
        "ago": int(now - r["created"]),
        "last_seen_ago": last_ago,
        "lat": last["lat"] if last else None, "lng": last["lng"] if last else None,
        "src": (last["src"] or "test") if last else None,
        "acc": round(last["acc"]) if last and last["acc"] is not None else None,
        "lost": (not closed) and last_ago is not None and last_ago > LOST_CONTACT_AFTER,
        "shared": bool(r["share_token"]),
        "trail": [[t["lat"], t["lng"]] for t in reversed(trail)] if trail_limit > 0 else [],
    }


def dist_m(a_lat, a_lng, b_lat, b_lng):
    """Distance in metres between two points."""
    r = 6371000
    p1, p2 = math.radians(a_lat), math.radians(b_lat)
    dp, dl = p2 - p1, math.radians(b_lng - a_lng)
    h = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(h))


def clean_point(c, iid, lat, lng, acc):
    """Hold the previous position when the new one is only GPS/Wi-Fi jitter, so a parked vehicle stays still."""
    last = c.execute("SELECT lat,lng,acc FROM locations WHERE incident_id=? ORDER BY id DESC LIMIT 1", (iid,)).fetchone()
    if not last:
        return lat, lng, acc
    threshold = max(MIN_MOVE_M, min(acc or 0, MAX_JITTER_M))
    if dist_m(last["lat"], last["lng"], lat, lng) < threshold:
        return last["lat"], last["lng"], (last["acc"] if last["acc"] is not None else acc)
    return lat, lng, acc


def get_beacon(c, device_id):
    """The beacon paired with this device, else the shared one ('*'). None if there is none."""
    return (c.execute("SELECT * FROM beacons WHERE device=?", (device_id,)).fetchone()
            or c.execute("SELECT * FROM beacons WHERE device='*'").fetchone())


def beacon_location(c, device_id, now):
    """(lat, lng, acc, src) from the laptop beacon, or None. src: laptop / manual / laptop_last."""
    b = get_beacon(c, device_id)
    if not b or now - b["ts"] > BEACON_LAST_KNOWN_S:
        return None
    if b["manual"]:
        src = "manual"
    elif now - b["ts"] <= BEACON_LIVE_S:
        src = "laptop"
    else:
        src = "laptop_last"
    return b["lat"], b["lng"], b["acc"], src


def seen(device_id, rssi=None, src=None):
    """Remember when a device was last heard from (stored, so a server restart keeps it)."""
    c = db()
    c.execute("INSERT INTO devices(device_id,ts,rssi,src) VALUES(?,?,?,?) "
              "ON CONFLICT(device_id) DO UPDATE SET ts=excluded.ts, "
              "rssi=COALESCE(excluded.rssi, devices.rssi), src=COALESCE(excluded.src, devices.src)",
              (device_id, time.time(), rssi, src))
    c.commit()
    c.close()


# ---------------------------------------------------------------- input checks
DEVICE_SRCS = ("gps", "phone", "test")   # location sources a device may report itself


class BadInput(ValueError):
    pass


def json_body():
    d = request.get_json(silent=True)
    if not isinstance(d, dict):
        raise BadInput("send a JSON object with Content-Type: application/json")
    return d


def clean_device_id(v):
    v = str(v or "").strip()
    if not v or len(v) > 20:
        raise BadInput("device_id is required (1-20 characters)")
    return v


def clean_number(d, key, lo, hi, required=True):
    v = d.get(key)
    if v is None or v == "":
        if required:
            raise BadInput(f"{key} is required")
        return None
    try:
        x = float(v)
    except (TypeError, ValueError):
        raise BadInput(f"{key} must be a number")
    if not math.isfinite(x) or not lo <= x <= hi:
        raise BadInput(f"{key} must be between {lo} and {hi}")
    return x


def read_alert(d):
    """Check and normalise an alert from a device. Raises BadInput with a clear message."""
    try:
        level = int(float(d.get("level", 1)))
    except (TypeError, ValueError):
        raise BadInput("level must be 1, 2 or 3")
    try:
        iid = int(d.get("incident_id") or 0)
    except (TypeError, ValueError):
        raise BadInput("incident_id must be a number")
    src = d.get("src", "test")
    return {
        "device_id": clean_device_id(d.get("device_id")),
        "incident_id": max(iid, 0),
        "level": min(max(level, 1), 3),            # only levels 1-3 exist
        "cancel": d.get("cancel") in (True, 1, "1", "true", "True"),
        "lat": clean_number(d, "lat", -90, 90),
        "lng": clean_number(d, "lng", -180, 180),
        "acc": clean_number(d, "acc", 0, 100000, required=False),
        "src": src if src in DEVICE_SRCS else "test",
        "phone": bool(d.get("phone")),
    }


@app.errorhandler(BadInput)
def bad_input(e):
    return jsonify(error=str(e)), 400


def lan_ip():
    """This laptop's address on the local network (used to build share links)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))   # no packet is sent; just picks the outgoing interface
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


# ---------------------------------------------------------------- pages
@app.get("/")
def police_page():
    return app.send_static_file("index.html")


@app.get("/phone")
def phone_page():
    return app.send_static_file("phone.html")


@app.get("/driver")
def driver_page():
    return app.send_static_file("driver.html")


@app.get("/track/<token>")
def track_page(token):
    return app.send_static_file("track.html")


# ---------------------------------------------------------------- device API (ESP8266 / phone)
@app.post("/api/heartbeat")
def heartbeat():
    d = json_body()
    rssi = clean_number(d, "rssi", -150, 0, required=False)
    seen(clean_device_id(d.get("device_id")), rssi=int(rssi) if rssi is not None else None)
    return jsonify(ok=True)


@app.post("/api/alert")
def alert():
    d = read_alert(json_body())
    c = db()
    now = time.time()
    device_id = d["device_id"]
    iid = d["incident_id"]
    row = c.execute("SELECT * FROM incidents WHERE id=? AND device_id=?", (iid, device_id)).fetchone() if iid else None
    if row and row["status"] == "closed":
        c.close()
        seen(device_id)
        return jsonify(closed=True, incident_id=iid)
    if not row:
        # device restarted and lost its incident id -> continue its open incident, but only a recent one;
        # an old forgotten incident must not swallow a brand-new alert
        row = c.execute("SELECT * FROM incidents WHERE device_id=? AND status!='closed' AND updated>=? "
                        "ORDER BY id DESC LIMIT 1", (device_id, now - REUSE_WINDOW_S)).fetchone()
    level = d["level"]
    if row:
        iid = row["id"]
    else:
        cur = c.execute("INSERT INTO incidents(device_id,level,status,cancel_requested,created,updated) "
                        "VALUES(?,?,?,?,?,?)", (device_id, level, "new", 0, now, now))
        iid = cur.lastrowid
    c.execute("UPDATE incidents SET level=MAX(level,?), updated=? WHERE id=?", (level, now, iid))
    if d.get("cancel"):
        c.execute("UPDATE incidents SET cancel_requested=1, cancel_at=COALESCE(cancel_at,?) WHERE id=?", (now, iid))

    lat, lng, src, acc = d["lat"], d["lng"], d["src"], d["acc"]
    if not d["phone"] and device_id != "PHONE-01" and src not in ("gps", "phone"):   # board without GPS: use the laptop beacon
        b = beacon_location(c, device_id, now)
        if b:
            lat, lng, acc, src = b
    if src != "test":                       # the simulated test drift is left as it is
        lat, lng, acc = clean_point(c, iid, lat, lng, acc)
    c.execute("INSERT INTO locations(incident_id,lat,lng,ts,src,acc) VALUES(?,?,?,?,?,?)",
              (iid, lat, lng, now, src, acc))
    c.commit()
    c.close()
    seen(device_id, src=src)
    return jsonify(closed=False, incident_id=iid)


@app.post("/api/driver_location")
def driver_location():
    """Position from the laptop's /driver page.
    Body: lat, lng, acc (m), fix_age (s since the browser got the fix), device (optional), manual (bool)."""
    d = json_body()
    now = time.time()
    lat = clean_number(d, "lat", -90, 90)
    lng = clean_number(d, "lng", -180, 180)
    device = str(d.get("device") or "*").strip().upper()[:20] or "*"
    manual = bool(d.get("manual"))
    acc = clean_number(d, "acc", 0, 100000, required=False)
    if acc is None and manual:
        acc = 10.0
    fix_age = clean_number(d, "fix_age", -3600, 10 ** 9, required=False) or 0
    if not manual and fix_age > BEACON_MAX_FIX_AGE_S:
        return jsonify(ok=True, used=False, reason="old fix")      # the browser kept re-sending an old position
    c = db()
    old = c.execute("SELECT * FROM beacons WHERE device=?", (device,)).fetchone()
    # skip a much worse reading while a recent, better one exists (Wi-Fi location sometimes jumps far away)
    if (not manual and old and not old["manual"] and acc and old["acc"] and now - old["ts"] < 20
            and acc > BAD_ACC_M and acc > 2 * old["acc"]):
        c.close()
        return jsonify(ok=True, used=False, reason="worse than the last reading")
    c.execute("INSERT INTO beacons(device,lat,lng,acc,manual,ts) VALUES(?,?,?,?,?,?) "
              "ON CONFLICT(device) DO UPDATE SET lat=excluded.lat, lng=excluded.lng, acc=excluded.acc, "
              "manual=excluded.manual, ts=excluded.ts", (device, lat, lng, acc, int(manual), now))
    c.commit()
    c.close()
    return jsonify(ok=True, used=True, device=device)


@app.post("/api/driver_location/stop")
def driver_location_stop():
    """Forget the beacon (e.g. when the laptop page is closed or a manual pin is cleared)."""
    d = request.get_json(silent=True) or {}
    device = str(d.get("device") or "*").strip().upper() or "*"
    c = db()
    c.execute("DELETE FROM beacons WHERE device=?", (device,))
    c.commit()
    c.close()
    return jsonify(ok=True)


# ---------------------------------------------------------------- police API
@app.get("/api/state")
def state():
    """Everything the police console needs, in one call (polled every 2 s)."""
    c = db()
    now = time.time()
    open_rows = c.execute("SELECT * FROM incidents WHERE status!='closed' ORDER BY id DESC").fetchall()
    hist_rows = c.execute("SELECT * FROM incidents WHERE status='closed' "
                          "ORDER BY closed_at DESC, id DESC LIMIT 100").fetchall()
    out = {
        "server_time": now,
        "devices": [{"device_id": v["device_id"], "ago": int(now - v["ts"]), "online": now - v["ts"] < ONLINE_TIMEOUT,
                     "rssi": v["rssi"]} for v in c.execute("SELECT * FROM devices ORDER BY device_id")],
        "beacons": [{"device": b["device"], "ago": int(now - b["ts"]), "manual": bool(b["manual"]),
                     "live": bool(b["manual"]) or now - b["ts"] <= BEACON_LIVE_S,
                     "acc": round(b["acc"]) if b["acc"] is not None else None}
                    for b in c.execute("SELECT * FROM beacons ORDER BY device")],
        "open": [incident_dict(c, r, now) for r in open_rows],
        "history": [incident_dict(c, r, now, trail_limit=0) for r in hist_rows],
    }
    c.close()
    return jsonify(out)


@app.get("/api/incidents/<int:iid>")
def incident(iid):
    c = db()
    r = c.execute("SELECT * FROM incidents WHERE id=?", (iid,)).fetchone()
    if not r:
        c.close()
        return jsonify(error="not found"), 404
    out = incident_dict(c, r, time.time(), trail_limit=1000)
    c.close()
    return jsonify(out)


@app.post("/api/incidents/<int:iid>/<action>")
def change_status(iid, action):
    if action not in ("ack", "dispatch", "close"):
        return jsonify(error="bad action"), 400
    c = db()
    r = c.execute("SELECT * FROM incidents WHERE id=?", (iid,)).fetchone()
    if not r:
        c.close()
        return jsonify(error="not found"), 404
    if r["status"] == "closed":
        c.close()
        return jsonify(error="already closed"), 409
    now = time.time()
    if action == "ack":
        c.execute("UPDATE incidents SET status=CASE WHEN status='new' THEN 'acknowledged' ELSE status END, "
                  "acknowledged_at=COALESCE(acknowledged_at,?), updated=? WHERE id=?", (now, now, iid))
    elif action == "dispatch":
        c.execute("UPDATE incidents SET status='dispatched', dispatched_at=COALESCE(dispatched_at,?), "
                  "acknowledged_at=COALESCE(acknowledged_at,?), updated=? WHERE id=?", (now, now, now, iid))
    else:
        body = request.get_json(silent=True) or {}
        outcome = body.get("outcome")
        if outcome not in ("resolved", "cancelled", "false_alarm"):
            outcome = "cancelled" if r["cancel_requested"] else "resolved"
        c.execute("UPDATE incidents SET status='closed', outcome=?, closed_at=?, updated=? WHERE id=?",
                  (outcome, now, now, iid))
    c.commit()
    out = incident_dict(c, c.execute("SELECT * FROM incidents WHERE id=?", (iid,)).fetchone(), now)
    c.close()
    return jsonify(out)


@app.post("/api/incidents/<int:iid>/share")
def share(iid):
    """Create (or reuse) a secret link that ground officers can open to follow the vehicle live."""
    c = db()
    r = c.execute("SELECT * FROM incidents WHERE id=?", (iid,)).fetchone()
    if not r:
        c.close()
        return jsonify(error="not found"), 404
    token = r["share_token"] or secrets.token_urlsafe(9)
    c.execute("UPDATE incidents SET share_token=? WHERE id=?", (token, iid))
    c.commit()
    c.close()
    host = request.host
    if host.startswith(("localhost", "127.")):        # a localhost link is useless on another phone
        host = f"{lan_ip()}:{request.host.split(':')[1] if ':' in request.host else PORT}"
    return jsonify(token=token, url=f"{request.scheme}://{host}/track/{token}")


@app.get("/api/track/<token>")
def track(token):
    c = db()
    r = c.execute("SELECT * FROM incidents WHERE share_token=?", (token,)).fetchone()
    if not r:
        c.close()
        return jsonify(error="link not valid"), 404
    out = incident_dict(c, r, time.time(), trail_limit=300)
    c.close()
    return jsonify(out)


init()

if __name__ == "__main__":
    print(f"\n  Police console:  http://localhost:{PORT}/   (other devices: http://{lan_ip()}:{PORT}/)")
    print(f"  Phone backup:    http://{lan_ip()}:{PORT}/phone")
    print(f"  Laptop GPS:      http://localhost:{PORT}/driver\n")
    print(f"  Database:        {DB}\n")
    app.run(host=HOST, port=PORT, threaded=True)