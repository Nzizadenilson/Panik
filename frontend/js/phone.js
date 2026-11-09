// Phone backup: sends the same alerts as the ESP8266 button
(() => {
  const $ = s => document.querySelector(s);
  let iid = 0, level = 0, burst = 0, burstStart = 0, cancel = false, timer = null, hold = null, gotFix = false;
  let pos = {lat: -1.9441, lng: 30.0619, acc: null};   // Kigali test location until the phone has GPS
  const st = (t, cls = '') => { const e = $('#status'); e.textContent = t; e.className = 'status ' + cls; };

  if (!window.isSecureContext) {
    $('#gps').textContent = 'Phone GPS is blocked on plain http pages, so a test location is used. ' +
      'Host the server with https to use the phone GPS.';
  } else if (navigator.geolocation) {
    navigator.geolocation.watchPosition(p => {
      pos = {lat: p.coords.latitude, lng: p.coords.longitude, acc: p.coords.accuracy}; gotFix = true;
      $('#gps').textContent = `Using phone GPS (~${Math.round(p.coords.accuracy)} m)`;
    }, e => { $('#gps').textContent = 'No GPS: ' + e.message + '. Using test location.'; }, {enableHighAccuracy: true});
  }

  async function send() {
    try {
      const d = await Panik.api('/api/alert', {method: 'POST', body: {
        device_id: 'PHONE-01', incident_id: iid, level, cancel,
        lat: pos.lat, lng: pos.lng, acc: pos.acc, src: gotFix ? 'phone' : 'test'}});
      iid = d.incident_id;
      if (d.closed) {
        clearInterval(timer); timer = null; level = 0; burst = 0; iid = 0; cancel = false;
        st('Incident closed by police. Back to standby.', 'ok');
        return;
      }
      st(`Alert active - Level ${level}${cancel ? ' - cancel requested' : ''}`, 'bad');
    } catch (e) { st('Could not reach server - retrying', 'bad'); }
  }

  $('#press').onclick = () => {
    const now = Date.now();
    if (!burst || now - burstStart > 3000) { burstStart = now; burst = 1; } else burst++;
    level = Math.max(level, Math.min(burst, 3));
    send();
    if (!timer) timer = setInterval(send, 3000);
  };
  const c = $('#cancel');
  c.onpointerdown = () => { hold = setTimeout(() => { if (timer) { cancel = true; send(); } }, 5000); };
  c.onpointerup = c.onpointerleave = () => clearTimeout(hold);
})();
