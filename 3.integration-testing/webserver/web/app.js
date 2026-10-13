/* Pico 2 W status page.
 *
 * The page itself is static and cached; everything live comes from two JSON
 * endpoints on the board:
 *
 *   GET  /api/status    one object, polled every 5 s
 *   GET  /api/networks  the last completed scan
 *   POST /api/scan      queue a scan
 *
 * The forms still work with this file blocked — they are ordinary POSTs to
 * /connect and /forget. This only intercepts them to stay on the page.
 */

'use strict';

const $ = (id) => document.getElementById(id);

const POLL_MS = 5000;
let pollTimer = null;
let scanWatch = false;

/* -- helpers --------------------------------------------------------------- */

function hhmmss(total) {
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  return `${h} h ${m} m ${s} s`;
}

/* -90 dBm is unusable, -50 is excellent. */
function bars(rssi) {
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -78) return 2;
  if (rssi >= -88) return 1;
  return 0;
}

function banner(text, warn) {
  const el = $('banner');
  if (!text) { el.hidden = true; return; }
  el.textContent = text;
  el.classList.toggle('warn', !!warn);
  el.hidden = false;
}

function live(ok) {
  $('live').classList.toggle('stale', !ok);
}

/* -- status ---------------------------------------------------------------- */

async function refreshStatus() {
  try {
    const r = await fetch('/api/status', { cache: 'no-store' });
    if (!r.ok) throw new Error(r.status);
    const s = await r.json();

    $('temp').textContent     = s.temp_c.toFixed(2);
    $('mode').textContent     = s.mode === 'ap' ? 'setup access point' : 'joined';
    $('network').textContent  = s.network || '—';
    $('ip').textContent       = s.ip;
    $('uptime').textContent   = hhmmss(s.uptime_s);
    $('requests').textContent = s.requests;

    $('forget').hidden = !s.stored;

    if (s.message) banner(s.message, s.mode === 'ap');
    $('scan').disabled    = s.scanning;
    $('scan').textContent = s.scanning ? 'Scanning…' : 'Scan';

    /* A scan we kicked off has finished — pull the new list once. */
    if (scanWatch && !s.scanning) {
      scanWatch = false;
      refreshNetworks();
    }

    live(true);
  } catch (e) {
    live(false);
  }
}

/* -- networks -------------------------------------------------------------- */

function renderNetworks(list) {
  const ul  = $('networks');
  const sel = $('ssid');

  ul.textContent = '';
  sel.textContent = '';

  if (!list.length) {
    const li = document.createElement('li');
    li.className = 'empty';
    li.textContent = 'No networks found — press Scan.';
    ul.append(li);

    const opt = document.createElement('option');
    opt.value = '';
    opt.textContent = '(scan to list networks)';
    sel.append(opt);
    return;
  }

  for (const ap of list) {
    const li = document.createElement('li');
    li.className = 'net';
    li.tabIndex = 0;
    li.setAttribute('role', 'option');

    const b = document.createElement('span');
    b.className = 'bars';
    const n = bars(ap.rssi);
    for (let i = 1; i <= 4; i++) {
      const bar = document.createElement('i');
      if (i <= n) bar.className = 'on';
      b.append(bar);
    }

    const name = document.createElement('span');
    name.className = 'name';
    name.textContent = ap.ssid;          /* textContent — never innerHTML */

    const lock = document.createElement('span');
    lock.className = 'lock';
    lock.textContent = ap.secured ? '🔒' : 'open';

    const meta = document.createElement('span');
    meta.className = 'meta';
    meta.textContent = `${ap.rssi} dBm · ch ${ap.channel}`;

    li.append(b, name, lock, meta);

    const choose = () => {
      sel.value = ap.ssid;
      $('other').value = '';
      for (const el of ul.children) el.setAttribute('aria-selected', 'false');
      li.setAttribute('aria-selected', 'true');
      $('pass').focus();
    };
    li.addEventListener('click', choose);
    li.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); choose(); }
    });

    ul.append(li);

    const opt = document.createElement('option');
    opt.value = ap.ssid;
    opt.textContent = `${ap.ssid} — ${ap.rssi} dBm`;
    sel.append(opt);
  }
}

async function refreshNetworks() {
  try {
    const r = await fetch('/api/networks', { cache: 'no-store' });
    if (!r.ok) throw new Error(r.status);
    renderNetworks(await r.json());
  } catch (e) {
    /* leave whatever is on screen */
  }
}

/* -- actions --------------------------------------------------------------- */

$('scan').addEventListener('click', async () => {
  $('scan').disabled = true;
  $('scan').textContent = 'Scanning…';
  try {
    await fetch('/api/scan', { method: 'POST' });
    scanWatch = true;
    setTimeout(refreshStatus, 500);
  } catch (e) {
    banner('Could not start a scan.', true);
    $('scan').disabled = false;
    $('scan').textContent = 'Scan';
  }
});

$('join').addEventListener('submit', async (e) => {
  e.preventDefault();

  const ssid = $('other').value.trim() || $('ssid').value;
  if (!ssid) { banner('Pick a network or type a name first.', true); return; }

  const btn = $('join').querySelector('button');
  btn.disabled = true;
  btn.textContent = 'Joining…';

  const body = new URLSearchParams({
    ssid: $('ssid').value,
    other: $('other').value,
    pass: $('pass').value,
  });

  try {
    await fetch('/connect', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: body.toString(),
    });
  } catch (err) {
    /* Expected: the board drops this network to join the new one. */
  }

  $('pass').value = '';
  banner(`Joining “${ssid}”. This page will stop updating — the board is ` +
         `moving to that network and its address will change. Watch the ` +
         `serial console, or rejoin the setup network if it fails.`);
  btn.disabled = false;
  btn.textContent = 'Join network';
});

$('forget').addEventListener('submit', async (e) => {
  e.preventDefault();
  if (!confirm('Erase the stored network and restart into setup mode?')) return;
  try {
    await fetch('/forget', { method: 'POST' });
  } catch (err) { /* the board is restarting */ }
  banner('Erasing the stored network and restarting into setup mode.');
});

/* -- go -------------------------------------------------------------------- */

refreshStatus();
refreshNetworks();
pollTimer = setInterval(refreshStatus, POLL_MS);

/* Stop hammering the board while the tab is in the background. */
document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    clearInterval(pollTimer);
  } else {
    refreshStatus();
    pollTimer = setInterval(refreshStatus, POLL_MS);
  }
});
