/* Pico 2 W setup portal.
 *
 * The page is a static file served from flash; everything live comes from
 * GET /api/json, polled every 3 s. The forms are ordinary POSTs to /wifi,
 * /ble, /scan and /reset, each of which answers 303 back to "/" — so with
 * this file blocked the portal still works, it just reloads instead of
 * updating in place.
 *
 * Every string in the JSON originates off the air (SSIDs, BLE device names)
 * and is inserted with textContent only. Never innerHTML here.
 */

'use strict';

const $ = (id) => document.getElementById(id);
const POLL_MS = 3000;
let timer = null;

/* -90 dBm unusable, -50 excellent. */
function bars(rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  if (rssi >= -88) return 1;
  return 0;
}

function barsEl(rssi) {
  const b = document.createElement('span');
  b.className = 'bars';
  const n = bars(rssi);
  for (let i = 1; i <= 4; i++) {
    const bar = document.createElement('i');
    if (i <= n) bar.className = 'on';
    b.append(bar);
  }
  return b;
}

function el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

function banner(text, warn) {
  const b = $('banner');
  if (!text) { b.hidden = true; return; }
  b.textContent = text;
  b.classList.toggle('warn', !!warn);
  b.hidden = false;
}

function pill(node, text, kind) {
  node.textContent = text;
  node.className = 'pill' + (kind ? ' ' + kind : '');
}

/* -- Wi-Fi ----------------------------------------------------------------- */

function renderWifi(w) {
  /* Keys are exactly what wifi_state_str() returns in wifi_manager.c. */
  const stateKind = {
    'connected':    'ok',
    'joining':      'busy',
    'access point': 'busy',
    'failed':       'bad',
    'idle':         '',
  }[w.state] || '';

  pill($('wifi-state'), w.state || '—', stateKind);
  $('wifi-ssid').textContent = w.ssid || '—';
  $('wifi-ip').textContent   = w.ip   || '—';

  if (w.fail_reason) {
    $('wifi-fail').textContent = w.fail_reason;
    $('wifi-fail').hidden = false;
  } else {
    $('wifi-fail').hidden = true;
  }

  const scanning = !!w.scanning;
  $('scan-btn').disabled    = scanning;
  $('scan-btn').textContent = scanning ? 'Scanning…' : 'Scan';

  const ul = $('wifi-list');
  ul.textContent = '';
  const list = w.scan || [];

  if (!list.length) {
    ul.append(el('li', 'empty', scanning ? 'Scanning…' : 'No scan yet.'));
    return;
  }

  for (const ap of list) {
    const li = el('li', 'row pick');
    li.tabIndex = 0;
    li.setAttribute('role', 'option');

    li.append(barsEl(ap.rssi));
    li.append(el('span', 'name', ap.ssid));
    if (ap.enterprise) li.append(el('span', 'tag ent', 'enterprise'));
    else if (ap.auth)  li.append(el('span', 'tag', ap.auth));
    li.append(el('span', 'meta', `${ap.rssi} dBm · ch ${ap.channel}`));

    const choose = () => {
      if (ap.enterprise) {
        banner(`“${ap.ssid}” looks like a WPA2-Enterprise network. ` +
               `The CYW43439 cannot join those.`, true);
        return;
      }
      $('ssid').value = ap.ssid;
      for (const r of ul.children) r.setAttribute('aria-selected', 'false');
      li.setAttribute('aria-selected', 'true');
      $('pass').focus();
    };
    li.addEventListener('click', choose);
    li.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); choose(); }
    });

    ul.append(li);
  }
}

/* -- Bluetooth ------------------------------------------------------------- */

function renderBle(b) {
  pill($('ble-link'), b.connected ? 'client connected' : 'advertising',
       b.connected ? 'ok' : '');

  const devices = b.devices || [];
  $('ble-count').textContent =
    devices.length ? `${devices.length} seen` : '—';

  /* pinned sensor */
  const pin = b.pinned;
  if (pin) {
    $('pinned').hidden = false;
    $('pin-name').textContent = pin.name || pin.addr;
    if (typeof pin.temperature_c === 'number') {
      $('pin-temp').hidden = false;
      $('pin-temp').firstElementChild.textContent = pin.temperature_c.toFixed(2);
    } else {
      $('pin-temp').hidden = true;
    }
    $('pin-meta').textContent =
      `${pin.addr} · ${pin.rssi} dBm · ${pin.seen} sighting${pin.seen === 1 ? '' : 's'}`;
  } else {
    $('pinned').hidden = true;
  }

  const ul = $('ble-list');
  ul.textContent = '';

  if (!devices.length) {
    ul.append(el('li', 'empty', 'Nothing seen yet.'));
    return;
  }

  for (const d of devices) {
    const li = el('li', 'row pick');
    li.tabIndex = 0;
    li.setAttribute('role', 'option');

    li.append(barsEl(d.rssi));
    li.append(el('span', 'name', d.name || d.addr));
    if (typeof d.temperature_c === 'number')
      li.append(el('span', 'tag temp', `${d.temperature_c.toFixed(1)}°C`));
    li.append(el('span', 'meta', `${d.rssi} dBm · ${d.seen}×`));

    const choose = () => {
      if (!d.name) {
        banner('That device advertises no name, so it cannot be pinned by name.',
               true);
        return;
      }
      $('target').value = d.name;
      for (const r of ul.children) r.setAttribute('aria-selected', 'false');
      li.setAttribute('aria-selected', 'true');
    };
    li.addEventListener('click', choose);
    li.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); choose(); }
    });

    ul.append(li);
  }
}

/* -- polling --------------------------------------------------------------- */

async function refresh() {
  try {
    const r = await fetch('/api/json', { cache: 'no-store' });
    if (!r.ok) throw new Error(r.status);
    const s = await r.json();

    renderWifi(s.wifi || {});
    renderBle(s.ble || {});

    if (s.notice) banner(s.notice, false);

    $('live').classList.remove('stale');
    $('foot-text').textContent =
      `Refreshes every 3 s · ${s.requests || 0} requests served`;
  } catch (e) {
    $('live').classList.add('stale');
  }
}

/* Submit via fetch so the page does not navigate, then poll for the result.
   On failure fall through to a normal submit — the board may have moved. */
function hook(formId, message) {
  const form = $(formId);
  if (!form) return;
  form.addEventListener('submit', async (e) => {
    e.preventDefault();
    const btn = form.querySelector('button');
    const was = btn.textContent;
    btn.disabled = true;
    btn.textContent = '…';
    try {
      await fetch(form.action, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams(new FormData(form)).toString(),
      });
      if (message) banner(message, false);
      setTimeout(refresh, 400);
    } catch (err) {
      banner('The board stopped answering — it may be switching networks. ' +
             'Watch the serial console for its new address.', true);
    }
    btn.disabled = false;
    btn.textContent = was;
    if (formId === 'wifi-form') $('pass').value = '';
  });
}

hook('scan-form',  null);
hook('wifi-form',  'Saving and joining — this may take a few seconds.');
hook('ble-form',   'Target saved.');

/* The reset form keeps a confirmation step. */
$('reset-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  if (!confirm('Erase stored settings and restart into access-point mode?')) return;
  try { await fetch('/reset', { method: 'POST' }); } catch (err) { /* restarting */ }
  banner('Settings erased. The board is restarting into access-point mode.', false);
});

refresh();
timer = setInterval(refresh, POLL_MS);

document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    clearInterval(timer);
  } else {
    refresh();
    timer = setInterval(refresh, POLL_MS);
  }
});
