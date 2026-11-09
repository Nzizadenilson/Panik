// Shared helpers for every Panik page
const Panik = {
  LEVEL_COLORS: {1: '#f5b700', 2: '#f07c1a', 3: '#e5383b'},
  SRC_LABEL: {gps: 'GPS', laptop: 'Laptop location', phone: 'Phone GPS', test: 'Test location (no GPS fix)'},
  STATUS_LABEL: {new: 'New - not accepted', acknowledged: 'Accepted', dispatched: 'Unit dispatched', closed: 'Closed'},
  OUTCOME_LABEL: {resolved: 'Resolved', cancelled: 'Cancelled (verified with driver)', false_alarm: 'False alarm'},

  esc(s) {
    return String(s ?? '').replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
  },
  ago(s) {
    if (s == null) return '-';
    if (s < 60) return s + 's';
    if (s < 3600) return Math.floor(s / 60) + ' min';
    if (s < 86400) return Math.floor(s / 3600) + ' h';
    return Math.floor(s / 86400) + ' days';
  },
  clock(ts) {
    if (!ts) return '-';
    const d = new Date(ts * 1000), today = new Date();
    const t = d.toLocaleTimeString([], {hour: '2-digit', minute: '2-digit', second: '2-digit'});
    return d.toDateString() === today.toDateString() ? t : d.toLocaleDateString([], {day: 'numeric', month: 'short'}) + ' ' + t;
  },
  async api(path, opts = {}) {
    const r = await fetch(path, {
      headers: {'Content-Type': 'application/json'}, cache: 'no-store', ...opts,
      body: opts.body ? JSON.stringify(opts.body) : undefined,
    });
    const data = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(data.error || ('HTTP ' + r.status));
    return data;
  },
  gmapsUrl(lat, lng) {
    return `https://www.google.com/maps/search/?api=1&query=${lat},${lng}`;
  },
  gmapsDirections(lat, lng) {
    return `https://www.google.com/maps/dir/?api=1&destination=${lat},${lng}`;
  },
  gmapsRoute(from, to) {
    const o = from ? `&origin=${from[0]},${from[1]}` : '';
    return `https://www.google.com/maps/dir/?api=1${o}&destination=${to[0]},${to[1]}&travelmode=driving`;
  },
  // Leaflet map with Streets / Satellite / Hybrid views; returns null (and shows a note) if Leaflet could not load
  makeMap(el, msgEl) {
    if (!window.L) {
      if (msgEl) {
        msgEl.hidden = false;
        msgEl.innerHTML = '<div><b>Map could not load.</b><br>The laptop/phone needs internet for the map ' +
          '(unpkg.com and the map tile servers). Alerts still work.</div>';
      }
      return null;
    }
    const map = L.map(el, {zoomControl: true}).setView([-1.9441, 30.0619], 13);   // Kigali
    const ESRI = 'https://server.arcgisonline.com/ArcGIS/rest/services/';
    const esri = (name, opts = {}) => L.tileLayer(ESRI + name + '/MapServer/tile/{z}/{y}/{x}', {maxZoom: 19, ...opts});
    const imgAttr = {attribution: 'Imagery &copy; Esri, Maxar, Earthstar Geographics'};
    const bases = {
      'Streets': L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',
        {maxZoom: 19, attribution: '&copy; OpenStreetMap contributors'}),
      'Satellite': esri('World_Imagery', imgAttr),
      'Hybrid': L.layerGroup([esri('World_Imagery', imgAttr), esri('Reference/World_Transportation'),
        esri('Reference/World_Boundaries_and_Places')]),
    };
    let saved = 'Streets';
    try { saved = localStorage.getItem('panikBase') || 'Streets'; } catch (e) {}
    (bases[saved] || bases.Streets).addTo(map);
    L.control.layers(bases, null, {position: 'topright', collapsed: window.innerWidth < 700}).addTo(map);
    L.control.scale({imperial: false}).addTo(map);
    map.on('baselayerchange', e => { try { localStorage.setItem('panikBase', e.name); } catch (err) {} });
    return map;
  },
};

// In-app road directions (OpenStreetMap routing via the public OSRM server) from a start point to the vehicle.
// The start is the browser's location when allowed, otherwise the officer taps the map (e.g. the police station).
Panik.Directions = class {
  constructor(map, panel, opts = {}) {
    this.map = map; this.panel = panel; this.opts = opts;
    this.start = null; this.target = null; this.line = null; this.startMarker = null;
    this.on = false; this.picking = false; this.lastCalc = 0; this.info = null;
    if (!map) return;
    map.on('click', e => {
      if (!this.picking) return;
      this.picking = false;
      map.getContainer().classList.remove('picking');
      this.setStart([e.latlng.lat, e.latlng.lng], 'Chosen start point');
    });
    panel.addEventListener('click', e => {
      const a = e.target.closest('[data-r]')?.dataset.r;
      if (a === 'change') this.askPick();
      else if (a === 'mine') this.useMyLocation();
      else if (a === 'clear') this.clear();
    });
  }
  show(html) { this.panel.hidden = false; this.panel.innerHTML = html; }
  begin(target) {
    if (!this.map) return;
    this.on = true; this.target = target;
    if (this.start) this.compute(true); else this.useMyLocation();
  }
  useMyLocation() {
    if (window.isSecureContext && navigator.geolocation) {
      this.show('<b>Directions</b><p>Finding your location...</p>');
      navigator.geolocation.getCurrentPosition(
        p => this.setStart([p.coords.latitude, p.coords.longitude], 'Your location'),
        () => this.askPick('Your location is not available.'),
        {enableHighAccuracy: true, timeout: 8000});
    } else this.askPick();
  }
  askPick(why = '') {
    this.picking = true;
    this.map.getContainer().classList.add('picking');
    this.show(`<b>Directions</b><p>${why} Tap the map where the unit starts from (for example the police station).</p>
      <div class="r-btns"><button class="btn small ghost" data-r="clear">Cancel</button></div>`);
  }
  setStart(ll, label) {
    this.start = ll; this.startLabel = label;
    if (!this.startMarker) {
      this.startMarker = L.circleMarker(ll, {radius: 9, weight: 3, color: '#fff', fillColor: '#2563eb', fillOpacity: 1})
        .addTo(this.map).bindTooltip('Start', {direction: 'top'});
    } else this.startMarker.setLatLng(ll);
    this.compute(true);
  }
  updateTarget(t) {
    this.target = t;
    if (this.on && this.start && !this.picking && Date.now() - this.lastCalc > 15000) this.compute(false);
  }
  async compute(fit) {
    if (!this.start || !this.target) return;
    this.lastCalc = Date.now();
    const [a, b] = [this.start, this.target];
    const url = `https://router.project-osrm.org/route/v1/driving/${a[1]},${a[0]};${b[1]},${b[0]}?overview=full&geometries=geojson`;
    try {
      const r = await (await fetch(url)).json();
      if (!r.routes || !r.routes.length) throw new Error('no route');
      const rt = r.routes[0];
      const pts = rt.geometry.coordinates.map(c => [c[1], c[0]]);
      if (!this.on) return;
      if (!this.line) this.line = L.polyline(pts, {color: '#2563eb', weight: 6, opacity: .8, dashArray: '10 8'}).addTo(this.map);
      else this.line.setLatLngs(pts);
      this.info = {km: rt.distance / 1000, min: Math.max(1, Math.round(rt.duration / 60))};
      this.render();
      if (fit) { this.map.fitBounds(this.line.getBounds(), {padding: [50, 50]}); this.opts.onFit && this.opts.onFit(); }
    } catch (e) {
      this.show(`<b>Directions</b><p>The route service could not be reached (it needs internet).
        Use Google Maps instead.</p>${this.btns()}`);
    }
  }
  btns() {
    return `<div class="r-btns">
      <a class="btn small green" target="_blank" rel="noopener" href="${Panik.gmapsRoute(this.start, this.target)}">Google Maps</a>
      <button class="btn small" data-r="change">Change start</button>
      ${window.isSecureContext ? '<button class="btn small" data-r="mine">My location</button>' : ''}
      <button class="btn small ghost" data-r="clear">Hide</button></div>`;
  }
  render() {
    const i = this.info;
    this.show(`<div class="r-head"><span class="r-big">${i.min} min</span><span>${i.km.toFixed(1)} km by road</span></div>
      <p class="r-sub">From: ${this.startLabel}. Updates as the vehicle moves.</p>${this.btns()}`);
  }
  clear() {
    this.on = false; this.picking = false;
    if (this.map) this.map.getContainer().classList.remove('picking');
    if (this.line) { this.map.removeLayer(this.line); this.line = null; }
    if (this.startMarker) { this.map.removeLayer(this.startMarker); this.startMarker = null; }
    this.start = null;
    this.panel.hidden = true;
  }
};

// Draws a vehicle's route snapped to real roads (OSRM map matching), with an accuracy circle.
// Falls back to straight lines between points when the matching service cannot be reached.
Panik.RoadTrail = class {
  constructor(map) { this.map = map; this.line = null; this.circle = null; this.reset(); }
  reset() { this.snapped = null; this.snapKey = ''; this.lastTry = 0; this.busy = false; }
  static dedupe(trail) {
    const out = [];
    for (const p of trail) {
      const q = out[out.length - 1];
      if (!q || Math.abs(q[0] - p[0]) > 1e-6 || Math.abs(q[1] - p[1]) > 1e-6) out.push(p);
    }
    return out;
  }
  update(trail, color, acc) {
    if (!this.map || !trail || !trail.length) return;
    const pts = Panik.RoadTrail.dedupe(trail).slice(-90);
    const last = pts[pts.length - 1];
    const key = pts.length + ':' + pts[0].join(',') + ':' + last.join(',');
    let draw = pts;
    if (this.snapped) {
      draw = this.snapped.slice();
      const end = draw[draw.length - 1];
      if (key !== this.snapKey && (Math.abs(end[0] - last[0]) > 1e-6 || Math.abs(end[1] - last[1]) > 1e-6)) draw.push(last);
    }
    const style = {color, weight: 5, opacity: .85};
    if (!this.line) this.line = L.polyline(draw, style).addTo(this.map);
    else this.line.setLatLngs(draw).setStyle(style);
    // accuracy circle: the real position is somewhere inside it
    if (acc && acc > 15) {
      if (!this.circle) this.circle = L.circle(last, {radius: acc, color, weight: 1, fillOpacity: .1, interactive: false}).addTo(this.map);
      else this.circle.setLatLng(last).setRadius(acc).setStyle({color});
    } else if (this.circle) { this.map.removeLayer(this.circle); this.circle = null; }
    if (pts.length >= 2 && key !== this.snapKey && !this.busy && Date.now() - this.lastTry > 8000) this.snap(pts, key);
  }
  async snap(pts, key) {
    this.busy = true; this.lastTry = Date.now();
    try {
      const coords = pts.map(p => p[1].toFixed(6) + ',' + p[0].toFixed(6)).join(';');
      const radiuses = pts.map(() => 50).join(';');
      const url = `https://router.project-osrm.org/match/v1/driving/${coords}` +
        `?geometries=geojson&overview=full&gaps=ignore&tidy=true&radiuses=${radiuses}`;
      const r = await (await fetch(url)).json();
      if (r.code !== 'Ok' || !r.matchings || !r.matchings.length) throw new Error(r.code);
      const out = [];
      r.matchings.forEach(m => m.geometry.coordinates.forEach(c => out.push([c[1], c[0]])));
      if (out.length >= 2) { this.snapped = out; this.snapKey = key; }
    } catch (e) { /* keep the straight-line trail */ }
    this.busy = false;
  }
  getBounds() { return this.line ? this.line.getBounds() : null; }
  remove() {
    if (this.line) { this.map.removeLayer(this.line); this.line = null; }
    if (this.circle) { this.map.removeLayer(this.circle); this.circle = null; }
    this.reset();
  }
};

