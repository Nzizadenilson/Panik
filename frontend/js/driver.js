// Sends this laptop's location to the server so ESP8266 alerts can use it
(() => {
  const st = (t, ok) => { const e = document.getElementById('status'); e.textContent = t; e.className = 'status ' + (ok ? 'ok' : 'bad'); };
  let last = null;

  async function push() {
    if (!last) return;
    try {
      await Panik.api('/api/driver_location', {method: 'POST', body: last});
      st(`Sending ${last.lat.toFixed(6)}, ${last.lng.toFixed(6)} (accuracy ~${Math.round(last.acc)} m) - ${new Date().toLocaleTimeString()}`, true);
    } catch (e) { st('Server not reachable', false); }
  }

  if (!window.isSecureContext) {
    st('Open this page as http://localhost:5000/driver on the laptop itself (browsers block location on plain http IP addresses).', false);
  } else if (!navigator.geolocation) {
    st('This browser has no location support.', false);
  } else {
    navigator.geolocation.watchPosition(
      p => { last = {lat: p.coords.latitude, lng: p.coords.longitude, acc: p.coords.accuracy}; push(); },
      e => st('Location error: ' + e.message + ' (try Firefox if Chromium fails)', false),
      {enableHighAccuracy: true, maximumAge: 0, timeout: 20000});
    setInterval(push, 3000);
  }
})();
