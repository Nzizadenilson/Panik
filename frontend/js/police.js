// Panik police console
(() => {
  const $ = s => document.querySelector(s);
  const P = Panik;

  let state = {open: [], history: [], devices: []};
  let tab = 'active';
  let histFilter = 'all';
  let focusId = null;        // incident shown in the detail view
  let focusFull = null;      // full data (whole route) for a closed incident
  let follow = true;         // keep the map centred on the focused vehicle
  let known = null;          // what we saw last poll, to detect new alerts
  let busy = false;

  // ------------------------------------------------------------ helpers
  const groupOf = i => i.status === 'closed' ? 'history'
    : i.cancel_requested ? 'cancel'
    : i.status === 'dispatched' ? 'dispatched' : 'active';
  const find = id => state.open.find(i => i.id === id) || state.history.find(i => i.id === id);
  const statusBadge = i => `<span class="badge ${i.status}">${P.STATUS_LABEL[i.status] || i.status}</span>`;
  const srcLabel = i => P.SRC_LABEL[i.src] || i.src || '-';

  function histMatch(i) {
    if (histFilter === 'dispatched') return !!i.dispatched_at;
    if (histFilter === 'accepted') return !!i.acknowledged_at && !i.dispatched_at;
    if (histFilter === 'cancelled') return i.outcome === 'cancelled' || !!i.cancel_at;
    return true;
  }

  // ------------------------------------------------------------ sound + toasts
  let soundOn = false, actx = null;
  try { soundOn = localStorage.getItem('panikSound') === '1'; } catch (e) {}
  function ensureAudio() {
    try {
      actx = actx || new (window.AudioContext || window.webkitAudioContext)();
      if (actx.state === 'suspended') actx.resume();
    } catch (e) {}
  }
  function beep(pattern) {
    if (!soundOn) return;
    ensureAudio();
    if (!actx) return;
    let t = actx.currentTime + 0.02;
    pattern.forEach(([freq, dur]) => {
      const o = actx.createOscillator(), g = actx.createGain();
      o.type = 'square'; o.frequency.value = freq; g.gain.value = 0.07;
      o.connect(g); g.connect(actx.destination);
      o.start(t); o.stop(t + dur); t += dur + 0.07;
    });
  }
  const SIREN = [[880, .18], [660, .18], [880, .18], [660, .18]];
  function updateSoundBtn() { $('#soundBtn').textContent = soundOn ? 'Sound on' : 'Sound off'; }
  $('#soundBtn').onclick = () => {
    soundOn = !soundOn;
    try { localStorage.setItem('panikSound', soundOn ? '1' : '0'); } catch (e) {}
    updateSoundBtn();
    if (soundOn) beep([[880, .12]]);
  };
  document.addEventListener('pointerdown', () => { if (soundOn) ensureAudio(); });
  updateSoundBtn();

  function toast(text, cls = '', id = null) {
    const t = document.createElement('div');
    t.className = 'toast ' + cls;
    t.textContent = text;
    t.onclick = () => { if (id) openDetail(id); t.remove(); };
    $('#toasts').prepend(t);
    setTimeout(() => t.remove(), 7000);
  }

  function detectChanges() {
    if (known) {
      for (const i of state.open) {
        const k = known.get(i.id);
        if (!k) {
          toast(`NEW ALERT: ${i.device_id} - Level ${i.level}`, 'l' + i.level, i.id);
          beep(i.level >= 3 ? SIREN.concat(SIREN) : SIREN);
        } else {
          if (i.level > k.level) { toast(`${i.device_id} raised to Level ${i.level}`, 'l' + i.level, i.id); beep(SIREN); }
          if (i.cancel_requested && !k.cancel) {
            toast(`${i.device_id} requested CANCEL - verify with the driver`, 'cancel', i.id); beep([[440, .35]]);
          }
          if (i.lost && !k.lost) { toast(`Lost contact with ${i.device_id}`, '', i.id); beep([[300, .5]]); }
        }
      }
    }
    known = new Map(state.open.map(i => [i.id, {level: i.level, cancel: i.cancel_requested, lost: i.lost}]));
  }

  // ------------------------------------------------------------ top bar
  function renderDevices() {
    if (!state.devices.length) return;
    $('#devices').innerHTML = state.devices.map(d =>
      `<span class="dev ${d.online ? '' : 'off'}" title="Last heard ${P.ago(d.ago)} ago">` +
      `<i></i>${P.esc(d.device_id)} ${d.online ? 'online' : 'OFFLINE'}</span>`).join('');
  }
  setInterval(() => { $('#clock').textContent = new Date().toLocaleTimeString(); }, 1000);

  function updateTitle() {
    const n = state.open.filter(i => i.status === 'new').length;
    document.title = n ? `(${n}) NEW ALERT - Panik` : 'Panik - Police console';
  }

  // ------------------------------------------------------------ list view
  function steps(i) {
    const s = (on, label, extra = '') => `<span class="step ${on ? 'done' : ''} ${extra}">${on ? '&#10003;' : '&#10007;'} ${label}</span>`;
    return `<div class="steps">${s(i.acknowledged_at, 'Accepted')}${s(i.dispatched_at, 'Dispatched')}` +
      (i.cancel_at ? s(true, 'Cancel requested', 'cancel') : '') + `</div>`;
  }

  function quickActions(i) {
    const g = groupOf(i);
    if (g === 'history') return `<button class="btn small" data-act="open">View route</button>`;
    if (g === 'cancel') return `<button class="btn purple" data-act="open">Review cancel</button>`;
    if (g === 'dispatched') return `<button class="btn blue" data-act="open">Track on map</button>`;
    if (i.status === 'new') return `<button class="btn primary" data-act="accept">Accept &amp; track</button>`;
    return `<button class="btn" data-act="open">Track on map</button><button class="btn blue" data-act="dispatch">Dispatch</button>`;
  }

  function cardHtml(i) {
    const open = i.status !== 'closed';
    const cls = open ? `l${i.level} ${i.status} ${i.lost ? 'lost' : ''}` : 'closed';
    return `<article class="card ${cls}" data-id="${i.id}">
      <div class="card-top">
        <span class="pill lvl-${i.level}">LEVEL ${i.level}</span>
        <span class="plate">${P.esc(i.device_id)}</span>
        <span class="time">${open ? P.ago(i.ago) + ' ago' : P.clock(i.closed_at)}</span>
      </div>
      <div class="card-mid">
        ${open ? statusBadge(i) : `<span class="badge ${i.outcome}">${P.OUTCOME_LABEL[i.outcome] || 'Closed'}</span>`}
        ${open && i.cancel_requested ? '<span class="badge cancel">Cancel requested</span>' : ''}
        ${open ? `<span class="muted">Updated ${P.ago(i.last_seen_ago)} ago &middot; ${srcLabel(i)}</span>`
               : `<span class="muted">Alert at ${P.clock(i.created)}</span>`}
      </div>
      ${i.lost ? `<div class="lost-msg">&#9888; LOST CONTACT - last seen ${P.ago(i.last_seen_ago)} ago</div>` : ''}
      ${open ? '' : steps(i)}
      <div class="card-actions">${quickActions(i)}</div>
    </article>`;
  }

  const EMPTY = {
    active: ['&#128737;', 'No active alerts', 'New panic alerts appear here.'],
    dispatched: ['&#128659;', 'No dispatched incidents', 'Incidents with a unit on the way appear here.'],
    cancel: ['&#9989;', 'No cancel requests', 'When a driver holds CANCEL, the incident moves here for you to verify.'],
    history: ['&#128450;', 'No closed incidents yet', 'Closed incidents are kept here.'],
  };

  function renderList() {
    const groups = {active: [], dispatched: [], cancel: [], history: state.history.filter(histMatch)};
    state.open.forEach(i => groups[groupOf(i)].push(i));
    groups.active.sort((a, b) => (b.status === 'new') - (a.status === 'new') || b.level - a.level || b.created - a.created);
    groups.cancel.sort((a, b) => b.created - a.created);

    const setCount = (k, n, hot) => { const el = $('#c-' + k); el.textContent = n; el.classList.toggle('hot', !!hot); };
    setCount('active', groups.active.length, groups.active.some(i => i.status === 'new'));
    setCount('dispatched', groups.dispatched.length);
    setCount('cancel', groups.cancel.length, groups.cancel.length);
    setCount('history', state.history.length);
    $('#histFilters').hidden = tab !== 'history';

    const items = groups[tab];
    $('#list').innerHTML = items.length ? items.map(cardHtml).join('')
      : `<div class="empty"><b>${EMPTY[tab][0]}</b><strong>${EMPTY[tab][1]}</strong><p>${EMPTY[tab][2]}</p></div>`;
  }

  $('#tabs').addEventListener('click', e => {
    const b = e.target.closest('.tab'); if (!b) return;
    tab = b.dataset.tab;
    document.querySelectorAll('.tab').forEach(t => t.classList.toggle('active', t === b));
    renderList();
  });
  $('#histFilters').addEventListener('click', e => {
    const b = e.target.closest('.chip'); if (!b) return;
    histFilter = b.dataset.f;
    document.querySelectorAll('.chip').forEach(c => c.classList.toggle('on', c === b));
    renderList();
  });
  $('#list').addEventListener('click', async e => {
    const card = e.target.closest('.card'); if (!card) return;
    const id = +card.dataset.id;
    const act = e.target.closest('[data-act]')?.dataset.act || 'open';
    if (act === 'accept') { await action(id, 'ack'); openDetail(id); }
    else if (act === 'dispatch') { await action(id, 'dispatch'); }
    else openDetail(id);
  });

  // ------------------------------------------------------------ detail view
  function focused() {
    if (focusFull && focusFull.id === focusId) return focusFull;
    return find(focusId);
  }

  function timeline(i) {
    const rows = [
      ['alert', 'Alert received', i.created],
      ['ack', 'Accepted by police', i.acknowledged_at],
      ['dispatch', 'Unit dispatched', i.dispatched_at],
      ['cancel', 'Driver requested cancel', i.cancel_at],
      ['close', i.closed_at ? 'Closed - ' + (P.OUTCOME_LABEL[i.outcome] || 'closed') : 'Closed', i.closed_at],
    ];
    return `<ol class="timeline">${rows.map(([k, label, ts]) =>
      `<li class="${ts ? 'done' : ''} t-${k}"><span>${label}</span><time>${P.clock(ts)}</time></li>`).join('')}</ol>`;
  }

  function detailActions(i) {
    const has = i.lat != null;
    if (i.status === 'closed') {
      return has ? `<a class="btn wide" target="_blank" rel="noopener" href="${P.gmapsUrl(i.lat, i.lng)}">Last location in Google Maps</a>` : '';
    }
    let h = '';
    if (i.status === 'new') h += `<button class="btn primary big wide" data-act="ack">&#10003; Accept incident</button>`;
    if (i.status !== 'dispatched') h += `<button class="btn blue big ${i.status === 'new' ? '' : 'wide'}" data-act="dispatch">&#128659; Dispatch unit</button>`;
    h += `<button class="btn green big ${i.status === 'new' ? '' : 'wide'}" data-act="share">&#128228; Share live location</button>`;
    if (has) h += `<button class="btn" data-act="route">&#129517; Directions</button>`;
    h += `<button class="btn" data-act="recenter">Re-centre map</button>`;
    if (i.cancel_requested) h += `<button class="btn purple wide" data-act="close-cancel">Driver verified safe - close as cancelled</button>`;
    h += `<button class="btn dark wide" data-act="close-resolved">Close incident - resolved</button>`;
    return h;
  }

  function renderDetail() {
    const i = focused();
    if (!i) { closeDetail(); return; }
    const open = i.status !== 'closed';
    $('#detailView').innerHTML = `
      <div class="detail-head">
        <button class="btn ghost small" data-act="back">&larr; All incidents</button>
        ${open ? `<button class="btn small ${follow ? 'dark' : ''}" data-act="follow">${follow ? 'Following vehicle' : 'Follow vehicle'}</button>` : ''}
      </div>
      <div class="detail-card ${open ? 'l' + i.level : ''}">
        <div class="detail-title">
          <span class="pill lvl-${i.level}">LEVEL ${i.level}</span>
          <h2>${P.esc(i.device_id)}</h2>
          ${open ? statusBadge(i) : `<span class="badge ${i.outcome}">${P.OUTCOME_LABEL[i.outcome] || 'Closed'}</span>`}
        </div>
        ${open && i.cancel_requested ? `<div class="notice cancel"><b>Cancel requested by the driver.</b> Call or check on
          the driver before closing - a driver under threat may be forced to press cancel.</div>` : ''}
        ${i.lost ? `<div class="notice lost">&#9888; LOST CONTACT - no update for ${P.ago(i.last_seen_ago)}.
          The map shows the last known position.</div>` : ''}
        <dl class="facts">
          <div><dt>Alert started</dt><dd>${P.clock(i.created)}${open ? ` (${P.ago(i.ago)} ago)` : ''}</dd></div>
          <div><dt>${open ? 'Last update' : 'Closed at'}</dt><dd>${open ? P.ago(i.last_seen_ago) + ' ago' : P.clock(i.closed_at)}</dd></div>
          <div><dt>Location source</dt><dd>${srcLabel(i)}${i.acc ? ` &middot; &plusmn;${i.acc} m` : ''}</dd></div>
          <div><dt>Coordinates</dt><dd>${i.lat != null ? i.lat.toFixed(5) + ', ' + i.lng.toFixed(5) : '-'}</dd></div>
        </dl>
        <h3>Timeline</h3>
        ${timeline(i)}
        <div class="detail-actions">${detailActions(i)}</div>
      </div>`;
  }

  async function openDetail(id) {
    if (focusId !== id) directions.clear();
    focusId = id; focusFull = null; follow = true;
    document.body.classList.add('detail-open');
    $('#listView').hidden = true;
    $('#detailView').hidden = false;
    if (history.state?.detail !== id) history.pushState({detail: id}, '');
    const i = find(id);
    if (!i || i.status === 'closed') {
      try { focusFull = await P.api('/api/incidents/' + id); } catch (e) { toast('Could not load incident: ' + e.message); }
    }
    renderDetail();
    renderMap(true);
    setTimeout(() => map && map.invalidateSize(), 250);
    window.scrollTo(0, 0);
  }

  function closeDetail(fromPop = false) {
    if (focusId === null) return;
    focusId = null; focusFull = null;
    directions.clear();
    document.body.classList.remove('detail-open');
    $('#detailView').hidden = true;
    $('#listView').hidden = false;
    lastFitKey = '';
    renderList();
    renderMap();
    setTimeout(() => map && map.invalidateSize(), 250);
    if (!fromPop && history.state?.detail) history.back();
  }
  window.addEventListener('popstate', () => closeDetail(true));

  $('#detailView').addEventListener('click', async e => {
    const act = e.target.closest('[data-act]')?.dataset.act;
    if (!act) return;
    const i = focused();
    if (act === 'back') closeDetail();
    else if (act === 'follow' || act === 'recenter') { follow = act === 'recenter' ? true : !follow; renderDetail(); renderMap(true); }
    else if (act === 'ack') action(i.id, 'ack');
    else if (act === 'dispatch') action(i.id, 'dispatch');
    else if (act === 'share') share(i);
    else if (act === 'route') {
      if (!map) { window.open(P.gmapsDirections(i.lat, i.lng), '_blank'); return; }
      if (window.innerWidth <= 860) window.scrollTo(0, 0);
      directions.begin([i.lat, i.lng]);
    }
    else if (act === 'close-resolved') {
      if (confirm(`Close incident for ${i.device_id} as RESOLVED?\nIt will move to History and the device returns to standby.`))
        { await action(i.id, 'close', {outcome: 'resolved'}); closeDetail(); }
    } else if (act === 'close-cancel') {
      if (confirm(`Have you confirmed with the driver of ${i.device_id} that they are safe?\nThe incident will be closed as CANCELLED.`))
        { await action(i.id, 'close', {outcome: 'cancelled'}); closeDetail(); }
    }
  });

  async function action(id, act, body) {
    try {
      await P.api(`/api/incidents/${id}/${act}`, {method: 'POST', body: body || {}});
      const msg = {ack: 'Incident accepted', dispatch: 'Unit dispatched', close: 'Incident closed and moved to History'}[act];
      toast(msg, 'ok');
    } catch (e) { toast('Action failed: ' + e.message); }
    await refresh();
  }

  // ------------------------------------------------------------ share live location
  const dlg = $('#shareDlg');
  async function share(i) {
    let url;
    try { url = (await P.api(`/api/incidents/${i.id}/share`, {method: 'POST'})).url; }
    catch (e) { toast('Could not create link: ' + e.message); return; }
    const text = `PANIK ALERT - vehicle ${i.device_id}, level ${i.level}. Follow its live location: ${url}`;
    $('#shareWhat').textContent = `Vehicle ${i.device_id} - Level ${i.level}`;
    $('#shareUrl').value = url;
    $('#waBtn').href = 'https://wa.me/?text=' + encodeURIComponent(text);
    $('#smsBtn').href = 'sms:?&body=' + encodeURIComponent(text);
    $('#openLinkBtn').href = url;
    const nb = $('#nativeShareBtn');
    nb.hidden = !navigator.share;
    nb.onclick = () => navigator.share({title: 'Panik live location', text, url}).catch(() => {});
    $('#copyBtn').textContent = 'Copy link';
    if (dlg.showModal) dlg.showModal(); else dlg.setAttribute('open', '');
  }
  $('#copyBtn').onclick = async () => {
    const input = $('#shareUrl');
    try { await navigator.clipboard.writeText(input.value); }
    catch (e) { input.select(); document.execCommand('copy'); }
    $('#copyBtn').textContent = 'Copied!';
  };
  $('#closeDlg').onclick = () => (dlg.close ? dlg.close() : dlg.removeAttribute('open'));

  // ------------------------------------------------------------ map
  const map = P.makeMap('map', $('#mapMsg'));
  const markers = {};            // incident id -> circle marker (open incidents)
  let endMarker = null, lastFitKey = '', roadFor = null;
  const road = map ? new P.RoadTrail(map) : null;
  if (map) map.on('dragstart', () => { if (focusId !== null && follow) { follow = false; renderDetail(); } });
  const directions = new P.Directions(map, $('#routePanel'),
    {onFit: () => { if (follow) { follow = false; renderDetail(); } }});

  function colorOf(i) { return i.status === 'closed' ? '#64748b' : i.lost ? '#111111' : P.LEVEL_COLORS[i.level]; }

  function renderMap(force = false) {
    if (!map) return;
    // one marker per open incident
    const ids = new Set();
    state.open.forEach(i => {
      if (i.lat == null) return;
      ids.add(i.id);
      let m = markers[i.id];
      if (!m) {
        m = L.circleMarker([i.lat, i.lng], {radius: 11, weight: 3, color: '#fff', fillOpacity: 1}).addTo(map);
        m.on('click', () => openDetail(i.id));
        markers[i.id] = m;
      }
      m.setLatLng([i.lat, i.lng]);
      m.setStyle({fillColor: colorOf(i), radius: i.id === focusId ? 15 : 11});
      m.bindTooltip(`${i.device_id} - L${i.level}`, {direction: 'top', offset: [0, -10], className: 'plate-tip'});
    });
    Object.keys(markers).forEach(id => { if (!ids.has(+id)) { map.removeLayer(markers[id]); delete markers[id]; } });

    // route of the focused incident
    const f = focusId !== null ? focused() : null;
    if (f && f.trail && f.trail.length) {
      if (roadFor !== f.id) { road.remove(); roadFor = f.id; }
      road.update(f.trail, colorOf(f), f.status === 'closed' ? null : f.acc);
      if (markers[f.id] && markers[f.id].bringToFront) markers[f.id].bringToFront();
      const last = f.trail[f.trail.length - 1];
      if (f.status === 'closed') {
        if (!endMarker) endMarker = L.circleMarker(last, {radius: 11, weight: 3, color: '#fff', fillColor: '#64748b', fillOpacity: 1}).addTo(map);
        endMarker.setLatLng(last);
        if (force && f.trail.length > 1) map.fitBounds(f.trail, {padding: [40, 40], maxZoom: 17});
        else if (force) map.setView(last, 16);
      } else {
        if (endMarker) { map.removeLayer(endMarker); endMarker = null; }
        directions.updateTarget(last);
        if (follow || force) map.setView(last, Math.max(map.getZoom(), 16));
      }
      return;
    }
    road.remove(); roadFor = null;
    if (endMarker) { map.removeLayer(endMarker); endMarker = null; }

    // overview: fit all open incidents when the set changes
    const pts = state.open.filter(i => i.lat != null).map(i => [i.lat, i.lng]);
    const key = state.open.map(i => i.id).join(',');
    if (key !== lastFitKey && pts.length) {
      lastFitKey = key;
      if (pts.length === 1) map.setView(pts[0], 15);
      else map.fitBounds(pts, {padding: [50, 50], maxZoom: 16});
    }
  }

  // ------------------------------------------------------------ polling
  async function refresh() {
    if (busy) return;
    busy = true;
    try {
      state = await P.api('/api/state');
      $('#conn').hidden = true;
      detectChanges();
      renderDevices();
      renderList();
      if (focusId !== null) {
        // a focused open incident that just got closed: load its full route for the history view
        if (!focusFull && find(focusId)?.status === 'closed') {
          try { focusFull = await P.api('/api/incidents/' + focusId); } catch (e) {}
        }
        renderDetail();
      }
      renderMap();
      updateTitle();
    } catch (e) {
      $('#conn').hidden = false;
    } finally { busy = false; }
  }
  refresh();
  setInterval(refresh, 2000);
})();
