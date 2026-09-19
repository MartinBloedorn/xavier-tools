/* xavier viewer -- top bar, widget layout, and the frame loop.
 *
 * Widgets are plain objects with four methods: buildBar, ingest, render and
 * reset. This module owns the DOM around them and the clock; a widget owns
 * its own buffers and its own canvas contents and nothing else.
 */

import { Stream, Store, setOsc } from './lib/stream.js';
import { DataWidget } from './widgets/data.js';
import { GyroWidget } from './widgets/gyro.js';

const WIDGETS = [DataWidget, GyroWidget];
/** No widget may be squeezed below a sixth of the window. */
const MIN_FRACTION = 1 / 6;

const $ = (sel) => document.querySelector(sel);

const dom = {
  stage: $('#stage'),
  empty: $('#empty'),
  port: $('#osc-port'),
  prefix: $('#osc-prefix'),
  apply: $('#osc-apply'),
  status: $('#status'),
  statusText: $('#status-text'),
  statusRate: $('#status-rate'),
  battery: $('#battery'),
  batteryFill: $('#battery-fill'),
  batteryText: $('#battery-text'),
  play: $('#play'),
  notice: $('#notice'),
  template: $('#tpl-widget'),
};

const ICON_PAUSE = '<rect x="3" y="2.5" width="3.5" height="11" rx="1"/><rect x="9.5" y="2.5" width="3.5" height="11" rx="1"/>';
const ICON_PLAY = '<path d="M4 2.6 13 8l-9 5.4z"/>';

const app = {
  meta: null,
  store: null,
  panels: new Map(),   // widget id -> { instance, element, canvas }
  ready: false,
};

// A deliberate handle for the console and for tools/cdp.py: everything else
// here is module-scoped, which is right for the app and useless for poking
// at a running viewer.
window.xavier = app;

// ---------------------------------------------------------------- startup

const stream = new Stream();

stream.on('init', (meta) => {
  if (!app.ready) {
    app.meta = meta;
    app.store = new Store(meta.config.ui);
    buildWidgets();
    bindTopBar();
    applyOscFields(meta.osc);
    applyCollapsed();
    rebuildStage();
    app.ready = true;
    requestAnimationFrame(frame);
  } else {
    // A reconnect. The configuration on disk may be behind what is on
    // screen -- the page is the owner -- so only the link details are
    // refreshed, and the buffers are dropped because they describe a
    // stream that has since had a gap in it.
    applyOscFields(meta.osc);
    for (const { instance } of app.panels.values()) instance.reset();
  }
});

stream.on('data', (msg) => {
  if (msg.status) applyStatus(msg.status);
  if (!app.ready) return;
  if (app.store.ui.paused) return;
  for (const { instance } of app.panels.values()) instance.ingest(msg);
});

stream.on('offline', () => {
  dom.status.className = 'status is-offline';
  dom.statusText.textContent = 'no server';
  dom.statusRate.textContent = '';
});

stream.connect();

// ------------------------------------------------------------------ setup

function buildWidgets() {
  // Every widget is instantiated up front, open or not, so that one opened
  // later already has history behind it instead of drawing a blank window
  // for the next several seconds.
  for (const Widget of WIDGETS) {
    const instance = new Widget({ meta: app.meta, store: app.store });
    const element = dom.template.content.firstElementChild.cloneNode(true);
    element.dataset.id = Widget.id;
    const canvas = element.querySelector('canvas');
    const bar = element.querySelector('.bar-body');
    instance.buildBar(bar);
    element.querySelector('.bar-toggle').addEventListener('click', () => {
      app.store.set('barsCollapsed', !app.store.ui.barsCollapsed);
      applyCollapsed();
    });
    app.panels.set(Widget.id, { instance, element, canvas });
  }
}

function layout() {
  // Tolerate a config from an older build, or a hand-edited one, that does
  // not mention a widget this build has.
  const saved = Array.isArray(app.store.ui.layout) ? app.store.ui.layout : [];
  const known = new Map(saved.map((entry) => [entry.id, entry]));
  const merged = [];
  for (const entry of saved) if (app.panels.has(entry.id)) merged.push(entry);
  for (const Widget of WIDGETS) {
    if (!known.has(Widget.id)) merged.push({ id: Widget.id, open: false, width: 1 });
  }
  return merged;
}

function bindTopBar() {
  const markDirty = () => { dom.apply.disabled = false; };
  dom.port.addEventListener('input', markDirty);
  dom.prefix.addEventListener('input', markDirty);
  dom.port.addEventListener('keydown', (e) => { if (e.key === 'Enter') applyOsc(); });
  dom.prefix.addEventListener('keydown', (e) => { if (e.key === 'Enter') applyOsc(); });
  dom.apply.addEventListener('click', applyOsc);

  dom.play.addEventListener('click', () => setPaused(!app.store.ui.paused));
  setPaused(!!app.store.ui.paused);

  for (const button of document.querySelectorAll('[data-widget]')) {
    button.addEventListener('click', () => toggleWidget(button.dataset.widget));
  }

  document.addEventListener('keydown', (event) => {
    if (event.target.matches('input, select, textarea')) return;
    if (event.code === 'Space') {
      event.preventDefault();
      setPaused(!app.store.ui.paused);
    }
  });

  // A debounced save loses the last change when the tab closes.
  window.addEventListener('pagehide', () => app.store.flush());
}

// ------------------------------------------------------------- top bar UI

function applyOscFields(osc) {
  if (document.activeElement !== dom.port) dom.port.value = osc.port;
  if (document.activeElement !== dom.prefix) dom.prefix.value = osc.prefix;
  dom.apply.disabled = true;
  if (osc.error) showNotice(osc.error);
}

async function applyOsc() {
  const port = parseInt(dom.port.value, 10);
  if (!(port >= 1 && port <= 65535)) {
    showNotice('port must be between 1 and 65535');
    return;
  }
  dom.apply.disabled = true;
  try {
    const result = await setOsc({ port, prefix: dom.prefix.value || '/' });
    applyOscFields(result.osc);
    for (const { instance } of app.panels.values()) instance.reset();
    clearNotice();
  } catch (error) {
    dom.apply.disabled = false;
    showNotice(String(error.message || error));
  }
}

function setPaused(paused) {
  app.store.set('paused', paused);
  dom.play.setAttribute('aria-pressed', String(paused));
  dom.play.querySelector('svg').innerHTML = paused ? ICON_PLAY : ICON_PAUSE;
  dom.play.querySelector('.btn-label').textContent = paused ? 'paused' : 'pause';
  dom.play.title = paused ? 'Resume the display (space)' : 'Pause the display (space)';
}

let noticeKey = null;

function showNotice(message) {
  if (noticeKey === message) return;
  noticeKey = message;
  dom.notice.textContent = message;
  dom.notice.hidden = false;
}

function clearNotice() {
  noticeKey = null;
  dom.notice.hidden = true;
}

function applyStatus(status) {
  dom.status.className = `status is-${status.state}`;
  dom.statusText.textContent = status.state;
  dom.statusRate.textContent = status.state === 'live' ? `${status.rate.toFixed(1)} Hz` : '';

  if (status.battery === null || status.battery === undefined) {
    dom.battery.className = 'battery is-unknown';
    dom.batteryText.textContent = '--';
  } else {
    const percent = Math.round(status.battery * 100);
    dom.battery.className = 'battery' + (percent <= 10 ? ' is-flat' : percent <= 25 ? ' is-low' : '');
    dom.batteryFill.style.width = `${Math.max(0, Math.min(100, percent))}%`;
    dom.batteryText.textContent = `${percent}%`;
  }

  // The single most common reason for an empty screen is a prefix that does
  // not match what the acquisition tool is sending. Say so, with the
  // prefixes actually arriving, rather than leaving it to be guessed at.
  if (!status.samples && status.unmatched && status.foreign.length) {
    showNotice(`OSC arriving on ${status.foreign.join(', ')} -- prefix mismatch?`);
  } else if (noticeKey && noticeKey.startsWith('OSC arriving')) {
    clearNotice();
  }
}

// ------------------------------------------------------------ stage layout

function toggleWidget(id) {
  const entries = layout();
  const entry = entries.find((e) => e.id === id);
  if (!entry) return;
  entry.open = !entry.open;
  app.store.set('layout', entries);
  rebuildStage();
}

function rebuildStage() {
  const entries = layout();
  const open = entries.filter((entry) => entry.open);

  for (const button of document.querySelectorAll('[data-widget]')) {
    const entry = entries.find((e) => e.id === button.dataset.widget);
    button.setAttribute('aria-pressed', String(!!(entry && entry.open)));
  }

  for (const child of Array.from(dom.stage.children)) {
    if (child !== dom.empty) child.remove();
  }
  dom.empty.hidden = open.length > 0;

  open.forEach((entry, index) => {
    if (index) dom.stage.appendChild(makeDivider(open[index - 1].id, entry.id));
    const panel = app.panels.get(entry.id);
    panel.element.style.flexGrow = String(entry.width > 0 ? entry.width : 1);
    panel.element.style.flexBasis = '0';
    dom.stage.appendChild(panel.element);
  });
}

function makeDivider(leftId, rightId) {
  const node = document.createElement('div');
  node.className = 'divider';
  node.setAttribute('role', 'separator');
  node.setAttribute('aria-orientation', 'vertical');
  node.title = 'Drag to resize';

  node.addEventListener('pointerdown', (event) => {
    event.preventDefault();
    // Capture keeps the drag alive when the cursor outruns the divider.
    // Guarded because a synthetic pointer -- from the CDP test harness --
    // has no active pointer to capture, and a throw here would abort the
    // handler before the move listener was attached.
    try { node.setPointerCapture(event.pointerId); } catch (e) { /* not fatal */ }
    node.classList.add('is-dragging');
    document.body.classList.add('is-resizing');

    const left = app.panels.get(leftId).element;
    const right = app.panels.get(rightId).element;
    const startX = event.clientX;
    const leftStart = left.getBoundingClientRect().width;
    const rightStart = right.getBoundingClientRect().width;
    const totalPx = leftStart + rightStart;
    const growTotal = parseFloat(left.style.flexGrow || 1) + parseFloat(right.style.flexGrow || 1);

    const move = (e) => {
      // The floor is a sixth of the *window*, per the spec -- not a sixth
      // of the pair -- so a third widget genuinely cannot be crushed. When
      // the pair itself is narrower than two floors, split it evenly
      // instead of letting the two clamps fight.
      let floor = window.innerWidth * MIN_FRACTION;
      if (totalPx < floor * 2) floor = totalPx / 2;
      let leftPx = leftStart + (e.clientX - startX);
      let rightPx = totalPx - leftPx;
      if (leftPx < floor) { leftPx = floor; rightPx = totalPx - floor; }
      if (rightPx < floor) { rightPx = floor; leftPx = totalPx - floor; }
      left.style.flexGrow = String((growTotal * leftPx) / totalPx);
      right.style.flexGrow = String((growTotal * rightPx) / totalPx);
    };

    const end = () => {
      try { node.releasePointerCapture(event.pointerId); } catch (e) { /* not fatal */ }
      node.classList.remove('is-dragging');
      document.body.classList.remove('is-resizing');
      node.removeEventListener('pointermove', move);
      node.removeEventListener('pointerup', end);
      node.removeEventListener('pointercancel', end);
      const entries = layout();
      for (const entry of entries) {
        const panel = app.panels.get(entry.id);
        if (panel && entry.open) entry.width = parseFloat(panel.element.style.flexGrow) || 1;
      }
      app.store.set('layout', entries);
    };

    node.addEventListener('pointermove', move);
    node.addEventListener('pointerup', end);
    node.addEventListener('pointercancel', end);
  });

  // Double-click restores an even split, which is far quicker than
  // dragging back to it.
  node.addEventListener('dblclick', () => {
    const entries = layout();
    for (const entry of entries) entry.width = 1;
    app.store.set('layout', entries);
    rebuildStage();
  });

  return node;
}

function applyCollapsed() {
  dom.stage.classList.toggle('bars-collapsed', !!app.store.ui.barsCollapsed);
}

// ------------------------------------------------------------- frame loop

function frame() {
  const paused = !!app.store.ui.paused;
  const entries = layout();
  for (const entry of entries) {
    if (!entry.open) continue;
    const panel = app.panels.get(entry.id);
    if (panel) panel.instance.render(panel.canvas, { paused });
  }
  requestAnimationFrame(frame);
}
