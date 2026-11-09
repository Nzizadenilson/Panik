import sqlite3, time
from flask import Flask, request, jsonify, Response

app = Flask(__name__)
DB = "panik.db"
DEFAULT_LAT, DEFAULT_LNG = -1.2921, 36.8219  # Nairobi, used by the phone page if no GPS


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
    c.commit()
    c.close()


@app.post("/api/alert")
def alert():
    d = request.get_json(force=True)
    c = db()
    now = time.time()
    iid = int(d.get("incident_id", 0))
    row = c.execute("SELECT * FROM incidents WHERE id=?", (iid,)).fetchone() if iid else None
    if row and row["status"] == "closed":
        c.close()
        return jsonify(closed=True, incident_id=iid)
    level = int(d.get("level", 1))
    if not row:
        cur = c.execute(
            "INSERT INTO incidents(device_id,level,status,cancel_requested,created,updated) VALUES(?,?,?,?,?,?)",
            (d.get("device_id", "?"), level, "new", 0, now, now))
        iid = cur.lastrowid
    c.execute("UPDATE incidents SET level=MAX(level,?), cancel_requested=MAX(cancel_requested,?), updated=? WHERE id=?",
              (level, 1 if d.get("cancel") else 0, now, iid))
    c.execute("INSERT INTO locations(incident_id,lat,lng,ts) VALUES(?,?,?,?)",
              (iid, float(d["lat"]), float(d["lng"]), now))
    c.commit()
    c.close()
    return jsonify(closed=False, incident_id=iid)


@app.post("/api/incidents/<int:iid>/<action>")
def change_status(iid, action):
    states = {"ack": "acknowledged", "dispatch": "dispatched", "close": "closed"}
    if action not in states:
        return jsonify(error="bad action"), 400
    c = db()
    c.execute("UPDATE incidents SET status=?, updated=? WHERE id=?", (states[action], time.time(), iid))
    c.commit()
    c.close()
    return jsonify(ok=True)


@app.get("/api/incidents")
def incidents():
    c = db()
    now = time.time()
    out = []
    for r in c.execute("SELECT * FROM incidents ORDER BY id DESC LIMIT 10").fetchall():
        trail = c.execute("SELECT lat,lng FROM locations WHERE incident_id=? ORDER BY id DESC LIMIT 50",
                          (r["id"],)).fetchall()
        out.append({"id": r["id"], "device_id": r["device_id"], "level": r["level"],
                    "status": r["status"], "cancel_requested": r["cancel_requested"],
                    "ago": int(now - r["created"]),
                    "trail": [[t["lat"], t["lng"]] for t in reversed(trail)]})
    c.close()
    return jsonify(out)


POLICE_HTML = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Panik - Police</title>
<link rel="stylesheet" href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css">
<script src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js"></script>
<style>
body{font-family:sans-serif;margin:0}
h1{background:#1a237e;color:#fff;margin:0;padding:10px;font-size:18px}
#map{height:45vh}
.card{margin:8px;padding:10px;border-radius:8px;color:#fff}
.card button{margin:6px 6px 0 0;padding:8px 12px;border:0;border-radius:6px}
.l1{background:#f1c40f;color:#000}.l2{background:#e67e22}.l3{background:#e74c3c}.closed{background:#95a5a6}
</style></head><body>
<h1>PANIK - POLICE</h1><div id="map"></div><div id="list">No alerts</div>
<script>
const map=L.map('map').setView([-1.2921,36.8219],13);
L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'&copy; OpenStreetMap'}).addTo(map);
let marker=null,line=null,lastId=null;
async function act(id,a){await fetch('/api/incidents/'+id+'/'+a,{method:'POST'});load();}
async function load(){
  const list=await (await fetch('/api/incidents')).json();
  const box=document.getElementById('list');box.innerHTML='';
  list.forEach(i=>{
    const closed=i.status==='closed';
    const d=document.createElement('div');
    d.className='card '+(closed?'closed':'l'+i.level);
    d.innerHTML='<b>'+(closed?'CLOSED':'LEVEL '+i.level)+'</b> | '+i.device_id+' | '+i.ago+'s ago<br>Status: '+i.status
      +(i.cancel_requested&&!closed?' <b>- CANCEL REQUESTED: verify with driver</b>':'')+'<br>'
      +(closed?'':'<button onclick="act('+i.id+',\\'ack\\')">Acknowledge</button>'
      +'<button onclick="act('+i.id+',\\'dispatch\\')">Dispatch</button>'
      +'<button onclick="act('+i.id+',\\'close\\')">Close incident</button>');
    box.appendChild(d);
  });
  const a=list.find(i=>i.status!=='closed'&&i.trail.length);
  if(a){
    const p=a.trail[a.trail.length-1];
    if(!marker){marker=L.marker(p).addTo(map);line=L.polyline(a.trail,{color:'red'}).addTo(map);}
    marker.setLatLng(p);line.setLatLngs(a.trail);
    if(lastId!==a.id){map.setView(p,16);lastId=a.id;}
  }else if(marker){map.removeLayer(marker);map.removeLayer(line);marker=null;line=null;lastId=null;}
}
load();setInterval(load,2000);
</script></body></html>"""

PHONE_HTML = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Panik backup</title>
<style>body{font-family:sans-serif;text-align:center;padding:20px}
button{width:90%;padding:40px;font-size:28px;margin:12px;border:0;border-radius:12px;color:#fff}
#p{background:#e74c3c}#c{background:#555}</style></head><body>
<h2>Panik - phone backup</h2>
<button id="p" onclick="press()">PRESS</button>
<button id="c">CANCEL (hold 5s)</button>
<p id="s">Ready</p>
<script>
let count=0,iid=0,cancel=false,timer=null,hold=null;
let pos={lat:-1.2921,lng:36.8219};
if(navigator.geolocation)navigator.geolocation.watchPosition(
  p=>{pos={lat:p.coords.latitude,lng:p.coords.longitude}},()=>{},{enableHighAccuracy:true});
const st=t=>document.getElementById('s').textContent=t;
async function send(){
  const r=await fetch('/api/alert',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({device_id:'PHONE-01',incident_id:iid,level:Math.min(count,3),cancel:cancel,lat:pos.lat,lng:pos.lng})});
  const d=await r.json();iid=d.incident_id;
  if(d.closed){clearInterval(timer);timer=null;count=0;iid=0;cancel=false;st('Incident closed by police');return;}
  st('Alert active. Level '+Math.min(count,3)+(cancel?' | cancel requested':''));
}
function press(){count++;send();if(!timer)timer=setInterval(send,3000);}
const c=document.getElementById('c');
c.onpointerdown=()=>{hold=setTimeout(()=>{if(timer){cancel=true;send();}},5000);};
c.onpointerup=c.onpointerleave=()=>clearTimeout(hold);
</script></body></html>"""


@app.get("/")
def police():
    return Response(POLICE_HTML, mimetype="text/html")


@app.get("/phone")
def phone():
    return Response(PHONE_HTML, mimetype="text/html")


if __name__ == "__main__":
    init()
    app.run(host="0.0.0.0", port=5000)