/* The live link to the backend, and the persisted UI state.
 *
 * Server-sent events in, JSON POSTs out. EventSource reconnects on its own,
 * so a viewer restarted during development simply resumes -- there is no
 * retry logic here on purpose.
 */

function emitter() {
  const map = new Map();
  return {
    on(name, fn) {
      if (!map.has(name)) map.set(name, []);
      map.get(name).push(fn);
      return () => this.off(name, fn);
    },
    off(name, fn) {
      const list = map.get(name);
      if (list) map.set(name, list.filter((f) => f !== fn));
    },
    emit(name, payload) {
      const list = map.get(name);
      if (list) for (const fn of list) fn(payload);
    },
  };
}

export class Stream {
  constructor() {
    this.events = emitter();
    this.source = null;
    this.online = false;
  }

  on(name, fn) { return this.events.on(name, fn); }

  connect() {
    const source = new EventSource('/api/stream');
    this.source = source;

    source.addEventListener('init', (event) => {
      this.online = true;
      this.events.emit('init', JSON.parse(event.data));
    });

    source.onmessage = (event) => {
      this.online = true;
      this.events.emit('data', JSON.parse(event.data));
    };

    source.onerror = () => {
      // EventSource fires this both for a transient drop it will retry and
      // for a hard close. Either way the honest report is "not connected";
      // if it reconnects, the next init/message says so.
      if (this.online) {
        this.online = false;
        this.events.emit('offline');
      }
    };
  }
}

async function postJSON(url, body) {
  const response = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(payload.error || `HTTP ${response.status}`);
  return payload;
}

/** Re-point the OSC input. Persisted server-side. */
export function setOsc(patch) {
  return postJSON('/api/osc', patch);
}

/** Emit one OSC message. Unused until the first analysis tool exists; see
 * the note in viewer/sender.py. */
export function emitOsc(address, args = []) {
  return postJSON('/api/emit', { address, args });
}

/** The `ui` subtree of viewer.conf.json, mirrored in the browser.
 *
 * The browser is the owner: it reads the saved state once at startup and
 * writes back whenever something changes. Writes are debounced because a
 * slider drag would otherwise be a few hundred POSTs and a few hundred
 * rewrites of the config file.
 */
export class Store {
  constructor(ui) {
    this.ui = ui;
    this.events = emitter();
    this.timer = null;
  }

  on(name, fn) { return this.events.on(name, fn); }

  section(name) { return this.ui[name]; }

  /** Merge `changes` into one widget's settings and persist. */
  patch(name, changes) {
    Object.assign(this.ui[name], changes);
    this.events.emit(name, this.ui[name]);
    this.events.emit('*', { section: name });
    this.save();
  }

  /** Set a top-level UI key (`paused`, `barsCollapsed`, `layout`). */
  set(key, value) {
    this.ui[key] = value;
    this.events.emit(key, value);
    this.events.emit('*', { section: key });
    this.save();
  }

  save() {
    clearTimeout(this.timer);
    this.timer = setTimeout(() => {
      postJSON('/api/config', { ui: this.ui }).catch(() => {
        // A failed save is not worth interrupting a live session for; the
        // state is still correct in the page, just not on disk.
      });
    }, 400);
  }

  /** Force an immediate write -- used on pagehide, where a debounce would
   * lose the last change. */
  flush() {
    clearTimeout(this.timer);
    const body = JSON.stringify({ ui: this.ui });
    if (navigator.sendBeacon) {
      navigator.sendBeacon('/api/config', new Blob([body], { type: 'application/json' }));
    }
  }
}
