/*
 * The page around qrstyle.wasm. Everything that decides what a QR code looks
 * like is in the C library (src/) -- this file only builds the string to
 * encode, hands it over, and copies the pixels onto a canvas.
 */
'use strict';

/* Same order as build_config() in web/qrstyle_web.c. */
const STYLES = [
  'Liquid', 'Dots', 'Teardrop', 'Diamond', 'Star', 'Night',
  'Classic', 'KC / Teal', 'KC / Crest', 'KC / Red', 'KC / Midnight',
];
const ECL = ['L', 'M', 'Q', 'H'];
/* Byte-mode capacity of a version 40 symbol at ECC HIGH, which a logo forces. */
const MAX_BYTES = 1273;
const RENDER_PX = 960;

/*---- Structured payloads ----*/

/* Wi-Fi and MECARD-style fields escape these with a backslash. */
const wifiEsc = (s) => s.replace(/([\\;,:"])/g, '\\$1');
/* vCard 3.0 text values. */
const vcardEsc = (s) => s.replace(/([\\;,])/g, '\\$1').replace(/\r?\n/g, '\\n');

const KINDS = {
  text: {
    fields: [{ id: 'text', label: 'Text', type: 'textarea', value: 'https://github.com/MRobertEvers/styled-qr-c' }],
    build: (v) => v.text,
  },
  url: {
    fields: [{ id: 'url', label: 'URL', type: 'url', placeholder: 'example.com/page', value: '' }],
    hint: 'https:// is added if the link has no scheme.',
    build: (v) => {
      const u = v.url.trim();
      if (!u) return '';
      return /^[a-z][a-z0-9+.-]*:/i.test(u) ? u : 'https://' + u;
    },
  },
  wifi: {
    fields: [
      { id: 'ssid', label: 'Network name (SSID)', value: '' },
      { id: 'password', label: 'Password', type: 'text', value: '' },
      { id: 'security', label: 'Security', type: 'select', value: 'WPA',
        options: [['WPA', 'WPA / WPA2 / WPA3'], ['WEP', 'WEP'], ['nopass', 'None (open)']] },
      { id: 'hidden', label: 'Hidden network', type: 'checkbox', value: false },
    ],
    hint: 'Phones offer to join the network when they scan this.',
    build: (v) => {
      if (!v.ssid) return '';
      let s = `WIFI:T:${v.security};S:${wifiEsc(v.ssid)};`;
      if (v.security !== 'nopass') s += `P:${wifiEsc(v.password)};`;
      if (v.hidden) s += 'H:true;';
      return s + ';';
    },
  },
  email: {
    fields: [
      { id: 'to', label: 'To', type: 'email', placeholder: 'someone@example.com', value: '' },
      { id: 'subject', label: 'Subject', value: '' },
      { id: 'body', label: 'Message', type: 'textarea', value: '' },
    ],
    build: (v) => {
      if (!v.to && !v.subject && !v.body) return '';
      const q = [];
      if (v.subject) q.push('subject=' + encodeURIComponent(v.subject));
      if (v.body) q.push('body=' + encodeURIComponent(v.body));
      return 'mailto:' + v.to.trim() + (q.length ? '?' + q.join('&') : '');
    },
  },
  phone: {
    fields: [{ id: 'number', label: 'Phone number', type: 'tel', placeholder: '+1 555 123 4567', value: '' }],
    build: (v) => {
      const n = v.number.replace(/[^\d+*#]/g, '');
      return n ? 'tel:' + n : '';
    },
  },
  sms: {
    fields: [
      { id: 'number', label: 'Phone number', type: 'tel', placeholder: '+1 555 123 4567', value: '' },
      { id: 'message', label: 'Message', type: 'textarea', value: '' },
    ],
    build: (v) => {
      const n = v.number.replace(/[^\d+*#]/g, '');
      if (!n) return '';
      return `SMSTO:${n}:${v.message}`;
    },
  },
  contact: {
    fields: [
      { row: [{ id: 'first', label: 'First name', value: '' }, { id: 'last', label: 'Last name', value: '' }] },
      { id: 'org', label: 'Organisation', value: '' },
      { row: [{ id: 'tel', label: 'Phone', type: 'tel', value: '' }, { id: 'email', label: 'Email', type: 'email', value: '' }] },
      { id: 'url', label: 'Website', type: 'url', value: '' },
    ],
    hint: 'A vCard 3.0 — phones offer to save it as a contact.',
    build: (v) => {
      if (![v.first, v.last, v.org, v.tel, v.email, v.url].some(Boolean)) return '';
      const fn = [v.first, v.last].filter(Boolean).join(' ') || v.org;
      const lines = ['BEGIN:VCARD', 'VERSION:3.0',
        `N:${vcardEsc(v.last)};${vcardEsc(v.first)};;;`, `FN:${vcardEsc(fn)}`];
      if (v.org) lines.push(`ORG:${vcardEsc(v.org)}`);
      if (v.tel) lines.push(`TEL:${v.tel.replace(/[^\d+*#]/g, '')}`);
      if (v.email) lines.push(`EMAIL:${v.email.trim()}`);
      if (v.url) lines.push(`URL:${v.url.trim()}`);
      lines.push('END:VCARD');
      return lines.join('\n');
    },
  },
  location: {
    fields: [
      { id: 'place', label: 'Address, place, or "lat, lon"', placeholder: '1 Main St, Springfield  —  or  39.0997, -94.5786', value: '' },
      { id: 'format', label: 'Format', type: 'select', value: 'maps',
        options: [['maps', 'Map link (opens anywhere)'], ['geo', 'geo: URI (coordinates only)']] },
    ],
    build: (v) => {
      const p = v.place.trim();
      if (!p) return '';
      const m = p.match(/^\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*$/);
      if (v.format === 'geo' && m) return `geo:${m[1]},${m[2]}`;
      return 'https://www.google.com/maps/search/?api=1&query=' + encodeURIComponent(p);
    },
  },
};

/*---- State ----*/

const store = {
  get(k, d) { try { const v = localStorage.getItem('sqr.' + k); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem('sqr.' + k, JSON.stringify(v)); } catch { /* private mode */ } },
};

const flat = (fields) => fields.flatMap((f) => f.row || [f]);
const values = {};
for (const [k, def] of Object.entries(KINDS)) {
  values[k] = Object.fromEntries(flat(def.fields).map((f) => [f.id, f.value]));
}
let kind = KINDS[store.get('kind', 'text')] ? store.get('kind', 'text') : 'text';
let style = Math.min(Math.max(0, store.get('style', 0) | 0), STYLES.length - 1);

/*---- wasm ----*/

let wasm = null;
const enc = new TextEncoder();
const dec = new TextDecoder();

function cstr(ptr) {
  if (!ptr) return '';
  const u = new Uint8Array(wasm.memory.buffer, ptr);
  let n = 0;
  while (u[n]) n++;
  return dec.decode(u.subarray(0, n));
}

/* Encode `text` in `styleIndex` into `canvas`. Returns the stats, or null if
 * the text does not fit. */
function renderInto(canvas, text, styleIndex, targetPx) {
  const bytes = enc.encode(text);
  if (bytes.length >= wasm.web_text_cap()) return null;
  const buf = new Uint8Array(wasm.memory.buffer, wasm.web_text(), bytes.length + 1);
  buf.set(bytes);
  buf[bytes.length] = 0;

  const px = wasm.web_render(styleIndex, targetPx);
  if (!px) return null;

  const rgba = new Uint8ClampedArray(wasm.memory.buffer, wasm.web_rgba(), px * px * 4);
  canvas.width = px;
  canvas.height = px;
  canvas.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(rgba), px, px), 0, 0);
  return {
    px,
    version: wasm.web_version(),
    ecl: ECL[wasm.web_ecl()],
    ppm: wasm.web_ppm(),
    ok: !!wasm.web_ok(),
    contrast: wasm.web_contrast_x10(),
    logo: wasm.web_logo_pct(),
    budget: wasm.web_logo_budget(),
    problem: cstr(wasm.web_problem()),
  };
}

/*---- UI ----*/

const $ = (id) => document.getElementById(id);
const canvas = $('qr');

function fieldEl(f) {
  const wrap = document.createElement('label');
  const v = values[kind][f.id];
  let input;

  if (f.type === 'checkbox') {
    wrap.className = 'check-field';
    input = document.createElement('input');
    input.type = 'checkbox';
    input.checked = !!v;
    input.addEventListener('change', () => { values[kind][f.id] = input.checked; update(); });
    wrap.append(input, document.createTextNode(f.label));
    return wrap;
  }

  wrap.className = 'field';
  const cap = document.createElement('span');
  cap.textContent = f.label;
  if (f.type === 'textarea') {
    input = document.createElement('textarea');
  } else if (f.type === 'select') {
    input = document.createElement('select');
    for (const [val, text] of f.options) input.add(new Option(text, val));
  } else {
    input = document.createElement('input');
    input.type = f.type || 'text';
    if (f.type === 'url' || f.type === 'email' || f.type === 'tel') input.spellcheck = false;
  }
  if (f.placeholder) input.placeholder = f.placeholder;
  input.value = v;
  input.addEventListener('input', () => { values[kind][f.id] = input.value; update(); });
  wrap.append(cap, input);
  return wrap;
}

function showKind(k) {
  kind = k;
  store.set('kind', k);
  for (const b of document.querySelectorAll('.kinds button')) {
    b.setAttribute('aria-selected', String(b.dataset.kind === k));
    b.tabIndex = b.dataset.kind === k ? 0 : -1;
  }
  const form = $('fields');
  form.replaceChildren();
  for (const f of KINDS[k].fields) {
    if (f.row) {
      const row = document.createElement('div');
      row.className = 'row';
      row.append(...f.row.map(fieldEl));
      form.append(row);
    } else {
      form.append(fieldEl(f));
    }
  }
  if (KINDS[k].hint) {
    const p = document.createElement('p');
    p.className = 'hint';
    p.textContent = KINDS[k].hint;
    form.append(p);
  }
  update();
}

function selectStyle(i) {
  style = i;
  store.set('style', i);
  for (const b of document.querySelectorAll('.style')) {
    const on = Number(b.dataset.style) === i;
    b.setAttribute('aria-checked', String(on));
    b.tabIndex = on ? 0 : -1;
  }
  update();
}

let pending = false;
function update() {
  if (pending) return;
  pending = true;
  requestAnimationFrame(() => { pending = false; draw(); });
}

function setStats(r) {
  $('s-version').textContent = r ? r.version : '–';
  $('s-ecl').textContent = r ? r.ecl : '–';
  $('s-contrast').textContent = r ? (r.contrast / 10).toFixed(1) + ':1' : '–';
  $('s-logo').textContent = r ? `${r.logo}% / ${r.budget}%` : '–';
}

function draw() {
  const payload = KINDS[kind].build(values[kind]);
  const n = enc.encode(payload).length;
  $('payload').textContent = payload || '(nothing yet)';
  $('bytes').textContent = `· ${n} / ${MAX_BYTES} bytes`;

  const empty = $('empty');
  const check = $('check');
  const actions = [$('download'), $('copy')];

  if (!wasm) return;

  if (!payload) {
    canvas.classList.add('stale');
    empty.hidden = false;
    empty.textContent = 'Fill in the fields to make a code.';
    check.textContent = '';
    check.className = 'check';
    setStats(null);
    actions.forEach((b) => (b.disabled = true));
    return;
  }

  const r = renderInto(canvas, payload, style, RENDER_PX);
  if (!r) {
    canvas.classList.add('stale');
    empty.hidden = false;
    empty.textContent = `Too long: ${n} bytes. With Tori in the middle the code needs high error correction, which holds at most ${MAX_BYTES} bytes.`;
    check.textContent = '';
    check.className = 'check';
    setStats(null);
    actions.forEach((b) => (b.disabled = true));
    return;
  }

  canvas.classList.remove('stale');
  empty.hidden = true;
  setStats(r);
  actions.forEach((b) => (b.disabled = false));
  if (r.ok) {
    check.className = 'check ok';
    check.textContent = `✓ Passes QrStyle_Check · ${r.px}×${r.px} px, ${r.ppm} px per module`;
  } else {
    check.className = 'check bad';
    check.textContent = `✗ ${r.problem || 'QrStyle_Check failed'}`;
  }
}

function pngBlob() {
  return new Promise((resolve) => canvas.toBlob(resolve, 'image/png'));
}

async function download() {
  const blob = await pngBlob();
  if (!blob) return;
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = `qr-${kind}-${STYLES[style].toLowerCase().replace(/[^a-z0-9]+/g, '-')}.png`;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

async function copy() {
  const b = $('copy');
  try {
    await navigator.clipboard.write([new ClipboardItem({ 'image/png': pngBlob() })]);
    b.textContent = 'Copied';
  } catch {
    b.textContent = 'Copy failed';
  }
  setTimeout(() => (b.textContent = 'Copy image'), 1400);
}

/* Arrow keys move along a tab list or radio group, as the ARIA patterns say. */
function arrowNav(container, selector, activate) {
  container.addEventListener('keydown', (e) => {
    const keys = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 };
    if (!(e.key in keys)) return;
    const items = [...container.querySelectorAll(selector)];
    const i = items.indexOf(document.activeElement);
    if (i < 0) return;
    e.preventDefault();
    const next = items[(i + keys[e.key] + items.length) % items.length];
    next.focus();
    activate(next);
  });
}

function buildStyleSwatches() {
  const box = $('styles');
  STYLES.forEach((name, i) => {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'style';
    b.dataset.style = i;
    b.setAttribute('role', 'radio');
    const c = document.createElement('canvas');
    renderInto(c, 'styled-qr-c', i, 0);
    const label = document.createElement('span');
    label.textContent = name;
    b.append(c, label);
    b.addEventListener('click', () => selectStyle(i));
    box.append(b);
  });
  arrowNav(box, '.style', (b) => selectStyle(Number(b.dataset.style)));
}

async function boot() {
  for (const b of document.querySelectorAll('.kinds button')) {
    b.addEventListener('click', () => showKind(b.dataset.kind));
  }
  arrowNav(document.querySelector('.kinds'), 'button', (b) => showKind(b.dataset.kind));
  $('download').addEventListener('click', download);
  if (window.ClipboardItem && navigator.clipboard && navigator.clipboard.write) {
    $('copy').addEventListener('click', copy);
  } else {
    $('copy').hidden = true;
  }
  showKind(kind);

  try {
    const src = await fetch('qrstyle.wasm');
    const { instance } = 'instantiateStreaming' in WebAssembly
      ? await WebAssembly.instantiateStreaming(src, {}).catch(async () =>
          WebAssembly.instantiate(await (await fetch('qrstyle.wasm')).arrayBuffer(), {}))
      : await WebAssembly.instantiate(await src.arrayBuffer(), {});
    wasm = instance.exports;
    wasm._initialize();
  } catch (err) {
    $('empty').hidden = false;
    $('empty').textContent = 'Could not load qrstyle.wasm: ' + err.message;
    return;
  }

  buildStyleSwatches();
  selectStyle(style);
}

boot();
