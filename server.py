"""Panik server: Flask API only. The web pages live in ./frontend."""
import math
import os
import secrets
import socket
import sqlite3
import time

from flask import Flask, request, jsonify

BASE = os.path.dirname(os.path.abspath(__file__))
DB = os.path.join(BASE, "panik.db")
app = Flask(__name__, static_folder=os.path.join(BASE, "frontend"), static_url_path="")

PORT = 5000
DRIVER_LOC = {"lat": None, "lng": None, "acc": None, "ts": 0}   # laptop acting as the vehicle GPS
DRIVER_LOC_MAX_AGE = 30   # s; older than this -> use the location the device sent
DEVICES = {}              # device_id -> {"ts", "rssi", "src"}
ONLINE_TIMEOUT = 75       # s without contact -> device offline
LOST_CONTACT_AFTER = 15   # s without a location update on an open incident -> lost contact
# Location cleaning: Wi-Fi/laptop positions jump around by tens of metres even when standing still.
MIN_MOVE_M = 15           # a new point must be at least this far from the last one to count as movement
MAX_JITTER_M = 100        # ...or further than its own accuracy, capped at this
BAD_ACC_M = 300           # readings worse than this are ignored when a recent better one exists

# columns added after the first version; added automatically to older databases
INCIDENT_COLS = {"acknowledged_at": "REAL", "dispatched_at": "REAL", "cancel_at": "REAL",
                 "closed_at": "REAL", "outcome": "TEXT", "share_token": "TEXT"}


# ---------------------------------------------------------------- database
def db():
    c = sqlite3.connect(DB)
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


def seen(device_id, **extra):
    info = DEVICES.setdefault(device_id, {})
    info.update(extra)
    info["ts"] = time.time()


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
    d = request.get_json(force=True)
    seen(d.get("device_id", "?"), rssi=d.get("rssi"), src=d.get("src"))
    return jsonify(ok=True)


@app.post("/api/alert")
def alert():
    d = request.get_json(force=True)
    c = db()
    now = time.time()
    device_id = d.get("device_id", "?")
    iid = int(d.get("incident_id", 0))
    row = c.execute("SELECT * FROM incidents WHERE id=? AND device_id=?", (iid, device_id)).fetchone() if iid else None
    if row and row["status"] == "closed":
        c.close()
        seen(device_id)
        return jsonify(closed=True, incident_id=iid)
    if not row:
        # device restarted and lost its incident id -> continue its open incident instead of a duplicate
        row = c.execute("SELECT * FROM incidents WHERE device_id=? AND status!='closed' ORDER BY id DESC LIMIT 1",
                        (device_id,)).fetchone()
    level = int(d.get("level", 1))
    if row:
        iid = row["id"]
    else:
        cur = c.execute("INSERT INTO incidents(device_id,level,status,cancel_requested,created,updated) "
                        "VALUES(?,?,?,?,?,?)", (device_id, level, "new", 0, now, now))
        iid = cur.lastrowid
    c.execute("UPDATE incidents SET level=MAX(level,?), updated=? WHERE id=?", (level, now, iid))
    if d.get("cancel"):
        c.execute("UPDATE incidents SET cancel_requested=1, cancel_at=COALESCE(cancel_at,?) WHERE id=?", (now, iid))

    lat, lng = float(d["lat"]), float(d["lng"])
    src = d.get("src", "test")
    acc = d.get("acc")
    if device_id != "PHONE-01" and src != "gps" and DRIVER_LOC["lat"] is not None \
            and now - DRIVER_LOC["ts"] < DRIVER_LOC_MAX_AGE:
        lat, lng, src, acc = DRIVER_LOC["lat"], DRIVER_LOC["lng"], "laptop", DRIVER_LOC["acc"]
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
    d = request.get_json(force=True)
    now = time.time()
    acc = float(d["acc"]) if d.get("acc") is not None else None
    old = DRIVER_LOC
    # skip a much worse reading while a recent, better one exists (Wi-Fi location sometimes jumps far away)
    if acc and old["acc"] and now - old["ts"] < 20 and acc > BAD_ACC_M and acc > 2 * old["acc"]:
        return jsonify(ok=True, used=False)
    DRIVER_LOC.update(lat=float(d["lat"]), lng=float(d["lng"]), acc=acc, ts=now)
    return jsonify(ok=True, used=True)


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
        "devices": [{"device_id": k, "ago": int(now - v["ts"]), "online": now - v["ts"] < ONLINE_TIMEOUT,
                     "rssi": v.get("rssi")} for k, v in sorted(DEVICES.items())],
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
    app.run(host="0.0.0.0", port=PORT, threaded=True)
