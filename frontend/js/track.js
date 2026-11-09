// Live-location page for officers on the ground (opened from a shared link)
(() => {
  const $ = s => document.querySelector(s);
  const P = Panik;
  const token = location.pathname.split('/').filter(Boolean).pop();
  const map = P.makeMap('map', $('#mapMsg'));
  let marker = null, follow = true, first = true, timer = null;
  const road = map ? new P.RoadTrail(map) : null;

  if (map) map.on('dragstart', () => { follow = false; });
  const directions = new P.Directions(map, $('#routePanel'), {onFit: () => { follow = false; }});
  let lastPos = null;
  $('#routeBtn').onclick = () => {
    if (!lastPos) return;
    if (!map) { window.open(P.gmapsDirections(lastPos[0], lastPos[1]), '_blank'); return; }
    directions.begin(lastPos);
  };
  $('#centerBtn').onclick = () => { follow = true; load(); };

  async function load() {
    let i;
    try { i = await P.api('/api/track/' + token); }
    catch (e) {
      $('#info').innerHTML = `<b>${e.message === 'link not valid' ? 'This link is not valid.' : 'Cannot reach the Panik server. Retrying...'}</b>`;
      return;
    }
    const open = i.status !== 'closed';
    $('#ended').hidden = open;
    $('#info').innerHTML = `
      <div class="row"><span class="pill lvl-${i.level}">LEVEL ${i.level}</span>
        <strong style="font-size:20px;letter-spacing:.04em">${P.esc(i.device_id)}</strong>
        <span class="badge ${i.status}">${P.STATUS_LABEL[i.status] || i.status}</span></div>
      <div class="row muted">${open ? `Updated ${P.ago(i.last_seen_ago)} ago` : 'Closed ' + P.clock(i.closed_at)}
        &middot; ${P.SRC_LABEL[i.src] || ''}${i.acc ? ' &middot; &plusmn;' + i.acc + ' m' : ''}</div>
      ${i.lost ? `<div class="lost-msg">&#9888; LOST CONTACT - showing last known position (${P.ago(i.last_seen_ago)} ago)</div>` : ''}`;
    if (i.lat != null) { $('#navBtn').href = P.gmapsDirections(i.lat, i.lng); if (!map) lastPos = [i.lat, i.lng]; }

    if (map && i.trail.length) {
      const color = !open ? '#64748b' : i.lost ? '#111' : P.LEVEL_COLORS[i.level];
      const last = i.trail[i.trail.length - 1];
      lastPos = last;
      if (open) directions.updateTarget(last);
      road.update(i.trail, color, open ? i.acc : null);
      if (!marker) marker = L.circleMarker(last, {radius: 13, weight: 3, color: '#fff', fillColor: color, fillOpacity: 1}).addTo(map);
      marker.setLatLng(last).setStyle({fillColor: color});
      marker.bringToFront && marker.bringToFront();
      if (follow || first) map.setView(last, Math.max(first ? 16 : map.getZoom(), 15));
      first = false;
    }
    if (!open && timer) { clearInterval(timer); timer = null; }
  }
  load();
  timer = setInterval(load, 3000);
})();
