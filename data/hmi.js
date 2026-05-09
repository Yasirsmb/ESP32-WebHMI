'use strict';

// ──────────────────────────────────────────────
//  State
// ──────────────────────────────────────────────
const pollTargets  = {};   // id → { el, barEl, unit, type, w }
const chartConfigs = {};   // sourceId → [{ chartId, w }]
const chartHistory = {};   // chartId → number[]
const lastSeen     = {};   // id → Date.now()
const activeAlarms = {};   // id → alarm message string
let   refreshMs    = 2000;
let   _theme       = {};   // colors from cfg.theme

// ──────────────────────────────────────────────
//  Utility
// ──────────────────────────────────────────────
function el(tag, className) {
  const e = document.createElement(tag);
  if (className) e.className = className;
  return e;
}
function flashBtn(btn) {
  btn.classList.add('success');
  setTimeout(() => btn.classList.remove('success'), 700);
}
function showToast(msg, isError = false) {
  const t = document.getElementById('toast');
  t.textContent = msg;
  t.style.color = isError ? '#f85149' : '#22c55e';
  t.classList.add('visible');
  clearTimeout(t._t);
  t._t = setTimeout(() => t.classList.remove('visible'), 3500);
}
function updateAlarms() {
  const bar = document.getElementById('alarms-text');
  if (!bar) return;
  const msgs = Object.values(activeAlarms);
  if (msgs.length === 0) {
    bar.textContent = 'No active alarms';
    bar.style.color = '';
  } else {
    bar.textContent = msgs.join('   │   ');
    bar.style.color = '#f85149';
  }
}

// ──────────────────────────────────────────────
//  Theme
// ──────────────────────────────────────────────
function applyTheme(theme) {
  if (!theme) return;
  _theme = theme;
  const r = document.documentElement.style;
  const set = (cssVar, key) => { if (theme[key]) r.setProperty(cssVar, theme[key]); };
  set('--bg',       'bg');
  set('--card-bg',  'card_bg');
  set('--border',   'border');
  set('--accent',   'accent');
  set('--label',    'label');
  set('--text-mid', 'text_mid');
  set('--text',     'text');
  set('--unit',     'unit');
  set('--ok',       'ok');
  set('--warn',     'warn');
  set('--crit',     'crit');
}

// ──────────────────────────────────────────────
//  Clock
// ──────────────────────────────────────────────
function updateClock() {
  const now = new Date();
  const pad = n => String(n).padStart(2, '0');
  const clk = document.getElementById('clock');
  if (clk) clk.textContent = pad(now.getHours()) + ':' + pad(now.getMinutes()) + ':' + pad(now.getSeconds());
}
setInterval(updateClock, 1000);
updateClock();

// ──────────────────────────────────────────────
//  Gauge helpers
// ──────────────────────────────────────────────
function polarXY(cx, cy, r, deg) {
  const rad = (deg - 90) * Math.PI / 180;
  return { x: cx + r * Math.cos(rad), y: cy + r * Math.sin(rad) };
}
function arcPath(cx, cy, r, a1, a2) {
  const s = polarXY(cx, cy, r, a1);
  const e = polarXY(cx, cy, r, a2);
  const large = (a2 - a1 > 180) ? 1 : 0;
  return `M${s.x.toFixed(2)} ${s.y.toFixed(2)} A${r} ${r} 0 ${large} 1 ${e.x.toFixed(2)} ${e.y.toFixed(2)}`;
}
function updateGauge(id, val, w) {
  const min = w.min ?? 0, max = w.max ?? 100;
  const pct   = Math.max(0, Math.min(1, (val - min) / (max - min)));
  const angle = -135 + pct * 270;
  const color = val >= (w.crit ?? Infinity) ? (_theme.crit || '#f85149')
              : val >= (w.warn ?? Infinity) ? (_theme.warn || '#f59e0b')
              : (w.color || _theme.ok || '#22c55e');
  const fill  = document.getElementById('arc_fill_' + id);
  const label = document.getElementById('gv_' + id);
  if (fill)  { fill.setAttribute('d', pct > 0.005 ? arcPath(100, 100, 70, -135, angle) : ''); fill.setAttribute('stroke', color); }
  if (label) { label.textContent = val.toFixed(val < 100 ? 1 : 0); label.setAttribute('fill', color); }
  const wEl = document.getElementById('w_' + id);
  if (wEl) {
    wEl.classList.toggle('alarm-active', val >= (w.crit ?? Infinity));
    wEl.classList.toggle('warn-active',  val >= (w.warn ?? Infinity) && val < (w.crit ?? Infinity));
  }
  // alarm tracking
  if (val >= (w.crit ?? Infinity)) {
    activeAlarms[id] = (w.label || id).toUpperCase() + ' CRIT ' + val.toFixed(1) + (w.unit ? ' ' + w.unit : '');
  } else if (val >= (w.warn ?? Infinity)) {
    activeAlarms[id] = (w.label || id).toUpperCase() + ' WARN ' + val.toFixed(1) + (w.unit ? ' ' + w.unit : '');
  } else {
    delete activeAlarms[id];
  }
  updateAlarms();
}

// ──────────────────────────────────────────────
//  Chart helpers
// ──────────────────────────────────────────────
function feedChart(chartId, val, w) {
  if (!chartHistory[chartId]) chartHistory[chartId] = [];
  chartHistory[chartId].push(val);
  if (chartHistory[chartId].length > (w.points || 60)) chartHistory[chartId].shift();
  drawChart(chartId, w);
}
function drawChart(chartId, w) {
  const cv = document.getElementById('cv_' + chartId);
  if (!cv) return;
  const ctx  = cv.getContext('2d');
  const data = chartHistory[chartId] || [];
  const cw = cv.offsetWidth || 400, ch = cv.offsetHeight || 110;
  cv.width = cw; cv.height = ch;
  ctx.clearRect(0, 0, cw, ch);
  ctx.strokeStyle = '#1a2338'; ctx.lineWidth = 1;
  [0, 0.25, 0.5, 0.75, 1].forEach(t => {
    const y = t * ch;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(cw, y); ctx.stroke();
  });
  const valid = data.filter(v => v !== null && !isNaN(v));
  if (valid.length < 2) return;
  const mn = Math.min(...valid), mx = Math.max(...valid), range = mx - mn || 1;
  ctx.strokeStyle = w.color || '#58a6ff'; ctx.lineWidth = 1.5; ctx.beginPath();
  let first = true;
  data.forEach((v, i) => {
    if (v === null || isNaN(v)) { first = true; return; }
    const x = (i / (data.length - 1)) * cw;
    const y = ch - ((v - mn) / range) * (ch - 16) - 8;
    first ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    first = false;
  });
  ctx.stroke();
  const last = valid[valid.length - 1];
  ctx.fillStyle = w.color || '#58a6ff';
  ctx.font = '11px monospace';
  ctx.fillText(last.toFixed(1) + (w.unit || ''), cw - 54, 13);
}

// ──────────────────────────────────────────────
//  Stale detection
// ──────────────────────────────────────────────
function markStale(id, stale) {
  const wEl = document.getElementById('w_' + id);
  if (!wEl) return;
  let badge = wEl.querySelector('.stale-badge');
  if (stale && !badge) {
    badge = el('div', 'stale-badge');
    badge.textContent = 'NO DATA — check key "' + id + '" in /api/status';
    wEl.appendChild(badge);
    showToast('Widget "' + id + '" missing from ESP32 status JSON', true);
  } else if (!stale && badge) {
    badge.remove();
  }
}

// ──────────────────────────────────────────────
//  Widget renderers
// ──────────────────────────────────────────────
function makeLabel(w) {
  const label = el('span', 'wcard-label');
  label.textContent = w.label || w.id;
  if (w.label_color) label.style.color = w.label_color;
  return label;
}

function applySize(card, w) {
  if (w.span && w.span > 1) card.style.gridColumn = 'span ' + w.span;
  if (w.rows && w.rows > 1) card.style.gridRow    = 'span ' + w.rows;
  if (w.height)             card.style.minHeight  = w.height + 'px';
}

// ──────────────────────────────────────────────
//  Drag-and-drop reordering
// ──────────────────────────────────────────────
let _dragCard = null;

function initDrag(card, w) {
  card.dataset.widgetId = w.id;
  card.setAttribute('draggable', 'true');

  card.addEventListener('dragstart', e => {
    _dragCard = card;
    e.dataTransfer.effectAllowed = 'move';
    setTimeout(() => card.classList.add('dragging'), 0);
  });

  card.addEventListener('dragend', () => {
    card.classList.remove('dragging');
    document.querySelectorAll('.wcard.drag-over').forEach(c => c.classList.remove('drag-over'));
    _dragCard = null;
  });

  card.addEventListener('dragover', e => {
    e.preventDefault();
    if (_dragCard && _dragCard !== card) card.classList.add('drag-over');
  });

  card.addEventListener('dragleave', e => {
    if (!card.contains(e.relatedTarget)) card.classList.remove('drag-over');
  });

  card.addEventListener('drop', e => {
    e.preventDefault();
    card.classList.remove('drag-over');
    if (!_dragCard || _dragCard === card) return;
    const grid = document.getElementById('grid');
    const cards = [...grid.children];
    const srcIdx = cards.indexOf(_dragCard);
    const dstIdx = cards.indexOf(card);
    if (srcIdx === -1 || dstIdx === -1) return;
    if (srcIdx < dstIdx) grid.insertBefore(_dragCard, card.nextSibling);
    else                 grid.insertBefore(_dragCard, card);
    saveOrder();
  });
}

function saveOrder() {
  if (!window._loadedConfig) return;
  const grid = document.getElementById('grid');
  const order = [...grid.children].map(c => c.dataset.widgetId).filter(Boolean);
  const widgetMap = {};
  window._loadedConfig.widgets.forEach(w => { widgetMap[w.id] = w; });
  window._loadedConfig.widgets = order.map(id => widgetMap[id]).filter(Boolean);
  fetch('/api/config', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(window._loadedConfig, null, 2)
  })
  .then(r => r.ok ? showToast('Layout saved') : showToast('Layout save failed', true))
  .catch(() => showToast('Layout not saved — device unreachable', true));
}

function renderInput(w) {
  const card = el('div', 'wcard');
  applySize(card, w); initDrag(card, w);
  const header = el('div', 'wcard-header');
  const label = makeLabel(w);
  header.append(label);
  const group = el('div', 'input-group');
  const input = el('input'); input.type = 'text'; input.placeholder = w.placeholder || '';
  const btn = el('button', 'btn-primary'); btn.textContent = w.button || 'SET';
  btn.onclick = () => sendControl(w.id, input.value.trim(), btn);
  input.addEventListener('keydown', e => { if (e.key === 'Enter') sendControl(w.id, input.value.trim(), btn); });
  group.append(input, btn); card.append(header, group);
  return card;
}

function renderButton(w) {
  const card = el('div', 'wcard');
  applySize(card, w); initDrag(card, w);
  const btn = el('button', 'btn-primary btn-block'); btn.textContent = w.label || w.id;
  btn.onclick = () => sendControl(w.id, '1', btn);
  card.append(btn);
  return card;
}

function renderToggle(w) {
  const card = el('div', 'wcard');
  applySize(card, w); initDrag(card, w);
  const header = el('div', 'wcard-header');
  const label = makeLabel(w);
  header.append(label);
  const row = el('div', 'toggle-row');
  const stateText = el('span', 'toggle-state'); stateText.textContent = 'OFF';
  const switchEl = el('label', 'switch');
  const chk = el('input'); chk.type = 'checkbox';
  if (w.state) { chk.checked = true; stateText.textContent = 'ON'; }
  const slider = el('span', 'slider-knob');
  chk.onchange = () => {
    stateText.textContent = chk.checked ? 'ON' : 'OFF';
    sendControl(w.id, chk.checked ? '1' : '0');
  };
  switchEl.append(chk, slider); row.append(stateText, switchEl); card.append(header, row);
  return card;
}

function renderSlider(w) {
  const card = el('div', 'wcard');
  applySize(card, w); initDrag(card, w);
  const header = el('div', 'wcard-header');
  const label = makeLabel(w);
  const valDisplay = el('span', 'slider-val'); valDisplay.textContent = (w.min ?? 0) + (w.unit || '');
  header.append(label, valDisplay);
  const range = el('input'); range.type = 'range'; range.min = w.min ?? 0;
  range.max = w.max ?? 100; range.value = w.min ?? 0; range.className = 'range-input';
  let debounce;
  range.oninput = () => {
    valDisplay.textContent = range.value + (w.unit || '');
    clearTimeout(debounce);
    debounce = setTimeout(() => sendControl(w.id, range.value), 300);
  };
  card.append(header, range);
  return card;
}

function renderDisplay(w) {
  const card = el('div', 'wcard');
  applySize(card, w); initDrag(card, w);
  card.id = 'w_' + w.id;
  const header = el('div', 'wcard-header');
  const label = makeLabel(w);
  const unit  = el('span', 'wcard-unit');  unit.textContent  = w.unit || '';
  header.append(label, unit);
  const val = el('div', 'display-big'); val.textContent = '—';
  const track = el('div', 'display-bar-track');
  const fill  = el('div', 'display-bar-fill');
  track.appendChild(fill);
  card.append(header, val, track);
  pollTargets[w.id] = { el: val, barEl: fill, unit: w.unit || '', type: 'display', w };
  return card;
}

function renderGauge(w) {
  const card = el('div', 'wcard');
  applySize(card, w); initDrag(card, w);
  card.id = 'w_' + w.id;
  card.style.display       = 'flex';
  card.style.flexDirection = 'column';
  const header = el('div', 'wcard-header');
  const label = makeLabel(w);
  const unit  = el('span', 'wcard-unit');  unit.textContent  = w.unit || '';
  header.append(label, unit);
  const NS  = 'http://www.w3.org/2000/svg';
  const svg = document.createElementNS(NS, 'svg');
  svg.setAttribute('viewBox', '0 0 200 130'); svg.setAttribute('class', 'gauge-svg');
  svg.style.flex      = '1';
  svg.style.width     = '100%';
  svg.style.height    = '0';
  svg.style.minHeight = '80px';
  const bgPath = document.createElementNS(NS, 'path');
  bgPath.setAttribute('d', arcPath(100, 100, 70, -135, 135));
  bgPath.setAttribute('stroke', '#1a2338'); bgPath.setAttribute('stroke-width', '12'); bgPath.setAttribute('fill', 'none');
  const fillPath = document.createElementNS(NS, 'path');
  fillPath.setAttribute('id', 'arc_fill_' + w.id);
  const gaugeOk = w.color || _theme.ok || '#22c55e';
  fillPath.setAttribute('stroke', gaugeOk); fillPath.setAttribute('stroke-width', '12');
  fillPath.setAttribute('fill', 'none'); fillPath.setAttribute('stroke-linecap', 'round');
  const valText = document.createElementNS(NS, 'text');
  valText.setAttribute('id', 'gv_' + w.id); valText.setAttribute('x', '100'); valText.setAttribute('y', '108');
  valText.setAttribute('text-anchor', 'middle'); valText.setAttribute('font-size', '28');
  valText.setAttribute('font-weight', 'bold'); valText.setAttribute('font-family', 'monospace'); valText.setAttribute('fill', gaugeOk);
  valText.textContent = '--';
  const minText = document.createElementNS(NS, 'text');
  minText.setAttribute('x', '22'); minText.setAttribute('y', '126');
  minText.setAttribute('font-size', '10'); minText.setAttribute('fill', '#3d5470'); minText.setAttribute('font-family', 'monospace');
  minText.textContent = w.min ?? 0;
  const maxText = document.createElementNS(NS, 'text');
  maxText.setAttribute('x', '178'); maxText.setAttribute('y', '126');
  maxText.setAttribute('text-anchor', 'end'); maxText.setAttribute('font-size', '10');
  maxText.setAttribute('fill', '#3d5470'); maxText.setAttribute('font-family', 'monospace');
  maxText.textContent = w.max ?? 100;
  svg.append(bgPath, fillPath, valText, minText, maxText);
  card.append(header, svg);
  pollTargets[w.id] = { type: 'gauge', unit: w.unit || '', w };
  return card;
}

function renderChart(w) {
  const card = el('div', 'wcard');
  if (!w.span) w.span = 3;
  applySize(card, w); initDrag(card, w);
  card.id = 'w_' + w.id;
  card.style.display = 'flex';
  card.style.flexDirection = 'column';
  const header = el('div', 'wcard-header');
  const label = makeLabel(w);
  header.append(label);
  const cv = document.createElement('canvas');
  cv.id = 'cv_' + w.id; cv.className = 'chart-canvas';
  cv.style.flex = '1';
  cv.style.minHeight = '80px';
  cv.style.height = '0';   // let flex-grow control height, not the element's intrinsic size
  chartHistory[w.id] = [];
  const src = w.source || w.id;
  if (!chartConfigs[src]) chartConfigs[src] = [];
  chartConfigs[src].push({ chartId: w.id, w });
  card.append(header, cv);
  return card;
}

function renderLed(w) {
  const card = el('div', 'wcard');
  applySize(card, w); initDrag(card, w);
  card.id = 'w_' + w.id;
  const header = el('div', 'wcard-header');
  const label  = makeLabel(w);
  header.append(label);
  const dot = el('div', 'led-dot');
  dot.id = 'led_' + w.id;
  if (w.color_off) dot.style.background = w.color_off;
  card.append(header, dot);
  pollTargets[w.id] = { type: 'led', ledEl: dot, w };
  return card;
}

const renderers = {
  input:   renderInput,
  button:  renderButton,
  toggle:  renderToggle,
  slider:  renderSlider,
  display: renderDisplay,
  gauge:   renderGauge,
  chart:   renderChart,
  led:     renderLed,
};

// ──────────────────────────────────────────────
//  Poll /api/status
// ──────────────────────────────────────────────
function pollStatus() {
  fetch('/api/status')
    .then(r => r.json())
    .then(data => {
      const now = Date.now();
      Object.entries(pollTargets).forEach(([id, target]) => {
        if (data[id] === undefined) return;
        const val = parseFloat(data[id]);
        lastSeen[id] = now;
        markStale(id, false);
        if (target.type === 'led') {
          const on = parseFloat(data[id]) !== 0;
          target.ledEl.classList.toggle('led-on', on);
          target.ledEl.style.background = on
            ? (target.w.color_on  || 'var(--ok)')
            : (target.w.color_off || '');
          target.ledEl.style.boxShadow = on
            ? ('0 0 12px ' + (target.w.color_on || 'var(--ok)'))
            : 'none';
        } else if (target.type === 'display') {
          target.el.textContent = data[id] + target.unit;
          if (target.barEl && target.w && target.w.min !== undefined && target.w.max !== undefined) {
            const pct = Math.max(0, Math.min(100, (val - target.w.min) / (target.w.max - target.w.min) * 100));
            target.barEl.style.width = pct + '%';
          }
        } else if (target.type === 'gauge') {
          updateGauge(id, val, target.w);
        }
        if (chartConfigs[id]) {
          chartConfigs[id].forEach(c => feedChart(c.chartId, val, c.w));
        }
      });
      Object.keys(pollTargets).forEach(id => {
        if (lastSeen[id] && (Date.now() - lastSeen[id]) > refreshMs * 3) {
          markStale(id, true);
        }
      });
    })
    .catch(() => {});
}

// ──────────────────────────────────────────────
//  Send command to /api/control
// ──────────────────────────────────────────────
function sendControl(id, value, btn) {
  if (value === '') return;
  fetch('/api/control', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id, value })
  })
  .then(r => r.ok ? r.json() : Promise.reject(r.status))
  .then(() => { if (btn) flashBtn(btn); showToast(id + ' → ' + value); })
  .catch(err => showToast('Error: ' + err, true));
}

// ──────────────────────────────────────────────
//  Config editor
// ──────────────────────────────────────────────
window.openEditor = function() {
  const overlay  = document.getElementById('editor-overlay');
  const textarea = document.getElementById('config-textarea');
  const status   = document.getElementById('editor-status');

  textarea.value     = '';
  status.textContent = 'Loading config from device…';
  status.style.color = '#7a94b0';
  overlay.style.display = 'flex';

  fetch('/api/config')
    .then(r => { if (!r.ok) throw new Error('HTTP ' + r.status); return r.text(); })
    .then(text => {
      const parsed = JSON.parse(text);
      window._loadedConfig = parsed;
      textarea.value     = JSON.stringify(parsed, null, 2);
      status.textContent = '';
    })
    .catch(err => {
      if (window._loadedConfig) {
        textarea.value     = JSON.stringify(window._loadedConfig, null, 2);
        status.textContent = 'Using cached copy (fetch failed: ' + err + ')';
        status.style.color = '#f59e0b';
      } else {
        status.textContent = 'Could not load config: ' + err;
        status.style.color = '#f85149';
      }
    });
};
window.closeEditor = function() {
  document.getElementById('editor-overlay').style.display = 'none';
};
window.saveConfig = async function() {
  const ta = document.getElementById('config-textarea');
  const st = document.getElementById('editor-status');
  let parsed;
  try {
    parsed = JSON.parse(ta.value);
  } catch(e) {
    st.textContent = 'JSON syntax error: ' + e.message;
    st.style.color = '#f85149'; return;
  }
  st.textContent = 'Saving to device…'; st.style.color = '#c0d0e0';
  try {
    const res = await fetch('/api/config', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(parsed, null, 2)
    });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    st.textContent = 'Saved! Reloading…'; st.style.color = '#22c55e';
    setTimeout(() => location.reload(), 900);
  } catch(e) {
    st.textContent = 'Save failed: ' + e.message;
    st.style.color = '#f85149';
  }
};

// ──────────────────────────────────────────────
//  Boot — fetch config and build UI
// ──────────────────────────────────────────────
fetch('/api/config')
  .then(r => r.json())
  .then(cfg => {
    window._loadedConfig = cfg;
    applyTheme(cfg.theme);
    document.getElementById('hmi-title').textContent = cfg.title || 'Machine Control Panel';
    refreshMs = cfg.refresh_ms ?? 2000;

    const grid = document.getElementById('grid');
    grid.style.gridAutoRows = 'minmax(150px, auto)';
    grid.innerHTML = '';

    (cfg.widgets || []).forEach(w => {
      const render = renderers[w.type];
      if (!render) { console.warn('Unknown widget type:', w.type); return; }
      grid.appendChild(render(w));
    });

    if (Object.keys(pollTargets).length > 0 || Object.keys(chartConfigs).length > 0) {
      pollStatus();
      setInterval(pollStatus, refreshMs);
    }
  })
  .catch(err => {
    document.getElementById('grid').innerHTML =
      '<div style="color:#f85149;padding:20px;font-size:0.75rem;letter-spacing:0.08em">FAILED TO LOAD CONFIG: ' + err + '</div>';
  });
