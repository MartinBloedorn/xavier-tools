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

/** The backend's OSC *output* endpoint, mirrored here.
 *
 * Two places need it -- the top bar, which edits it, and the analysis
 * widget, which must not post results into a disabled sender at its send
 * rate just to have them dropped. Plain object rather than another emitter:
 * the widget reads it inside a frame it is already drawing, and a
 * subscription would buy nothing over reading the field.
 */
export const output = {
  enabled: false, host: '127.0.0.1', port: 9100, prefix: '/xavier',
};

/** Record the endpoint the backend reports -- from `init`, or from a reply
 * to a change made here. */
export function noteOutput(stats) {
  if (stats) Object.assign(output, stats);
}

/** Change the OSC output endpoint. Persisted server-side. */
export async function setOutput(patch) {
  const result = await postJSON('/api/output', patch);
  noteOutput(result.output);
  return result;
}

/** Emit OSC on behalf of an analysis tool.
 *
 * Takes the whole result set at once: the values describe one instant and
 * should travel together, and one request per value at the send rate would
 * be four times the traffic for nothing.
 */
export async function emitOsc(messages) {
  const result = await postJSON('/api/emit', { messages });
  noteOutput(result.output);
  return result;
}

/** One widget's outbound stream: a rate limit, a queue depth of one, and a
 * record of what actually went out.
 *
 * Two widgets emit their own results now, and both want the same three
 * things, none of which is obvious:
 *
 *   - only one request in flight at a time, because a slow reply must not
 *     build a queue of stale values, each going out later than the one
 *     after it is worth;
 *   - a *measured* rate as well as the configured one -- a batch is one
 *     POST and the browser's round trip is therefore added to every period,
 *     so a stream asked for 30 Hz may well be delivering 7;
 *   - the last error kept rather than thrown, since the caller is a frame
 *     of a live display with nowhere to put an exception.
 */
export class OscOut {
  constructor() {
    this.lastSend = 0;
    this.rate = 0;       // measured, not the setting
    this.pending = false;
    this.error = null;
  }

  /** True when nothing has gone out recently enough for `rate` to mean
   * anything. */
  get idle() {
    return !this.rate || performance.now() / 1000 - this.lastSend > 2;
  }

  /** Send at most `hz` times a second. `build` is called only when a batch
   * is actually due, and may return nothing to skip this one -- values that
   * are not ready yet are better skipped than sent as zeros. */
  send(wall, hz, build) {
    if (!output.enabled || this.pending) return;
    const period = 1 / Math.min(60, Math.max(1, hz));
    if (wall - this.lastSend < period) return;
    const messages = build();
    if (!messages || !messages.length) return;

    const gap = wall - this.lastSend;
    // Ignore the first send and any gap long enough to be a resumption
    // rather than a period: neither describes the rate of the stream.
    if (this.lastSend && gap > 0 && gap < 5) {
      this.rate = this.rate ? this.rate + (1 / gap - this.rate) * 0.2 : 1 / gap;
    }
    this.lastSend = wall;
    this.pending = true;
    emitOsc(messages)
      .then(() => { this.error = null; })
      .catch((error) => { this.error = String(error.message || error); })
      .finally(() => { this.pending = false; });
  }

  /** What to show about this stream, or null when it is not running.
   * `sending` is the widget's own switch; the top bar's is `output`.
   *
   * A level rather than a colour: this module knows about the link, not
   * about the stylesheet, and the widget drawing the line has the palette
   * in hand already.
   */
  status(sending) {
    if (!sending) return null;
    if (!output.enabled) return { text: 'osc output off', level: 'warn' };
    if (this.error) return { text: `osc: ${this.error}`, level: 'bad' };
    const rate = this.idle ? 'idle' : `${this.rate.toFixed(1)} Hz`;
    return { text: `${output.prefix} · ${rate}`, level: 'faint' };
  }
}

/** Round for the wire: three decimals is finer than any of these indices is
 * meaningful to, and keeps the JSON small at the send rate. */
export const round3 = (v) => Math.round(v * 1000) / 1000;

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
