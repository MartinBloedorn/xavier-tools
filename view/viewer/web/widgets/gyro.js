/* The gyro viewer: head motion, and an estimate of where the head is.
 *
 * The gyro reports *rate*, so position has to be integrated out of it -- and
 * a plain integrator walks off to infinity on the smallest bias. What is
 * used here is a leaky integrator, which is the same thing as a first-order
 * high pass on the integral:
 *
 *     angle[n] = a * angle[n-1] + rate * dt,   a = exp(-2*pi*fc*dt)
 *
 * so the estimate tracks real movement and then relaxes back to zero with a
 * time constant of 1/(2*pi*fc). A constant bias therefore parks the output
 * at a constant offset instead of ramping without bound.
 *
 * Units are deliberately "a.u.". The EPOC's gyro scale is undocumented and
 * the zero offsets in the protocol layer (102/104) are inherited from
 * emokit without justification -- see epoc/doc/protocol.md. Calling the
 * output degrees would be inventing a calibration that does not exist.
 */

import { SampleRing } from '../lib/ring.js';
import * as ui from '../lib/controls.js';
import { OscOut, round3 } from '../lib/stream.js';
import {
  FONT, FONT_LABEL, Smoothed, fmt, fitCanvas, hline, palette, placeholder, text, vline,
} from '../lib/plot.js';

const VALUE_FONT = '15px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';

const MAX_WINDOW = 60;
const WINDOW_CHOICES = [4, 8, 12, 24, 60];
const SEND_CHOICES = [5, 10, 20, 30];
const RATE_X = 0, RATE_Y = 1, ANGLE_X = 2, ANGLE_Y = 3;
const TRAIL_SECONDS = 2.5;

const COLOR_RATE = palette.rate;
const COLOR_ANGLE = palette.angle;

export class GyroWidget {
  static id = 'gyro';
  static title = 'gyro';

  constructor({ meta, store }) {
    this.meta = meta;
    this.store = store;
    this.settings = store.section('gyro');

    // Four tracks: gyro x, gyro y, yaw, pitch. The derived pair lives in
    // the same ring so a change to the high-pass can be applied to the
    // whole visible history rather than only to samples yet to arrive.
    this.ring = new SampleRing(4, Math.ceil(MAX_WINDOW * meta.sampleRate) + 8);
    this.yaw = 0;
    this.pitch = 0;
    this.biasX = 0;
    this.biasY = 0;
    this.lastT = NaN;
    this.displayTime = NaN;

    this.rateRange = new Smoothed(1);
    this.angleRange = new Smoothed(1);
    this.bubbleRate = new Smoothed(1);
    this.bubbleAngle = new Smoothed(1);
    this.osc = new OscOut();
  }

  // -- data ---------------------------------------------------------------

  ingest(msg) {
    const gyro = msg.gyro;
    if (!gyro) return;
    const { t, x, y } = gyro;
    const fc = this.settings.highpassHz;
    const gain = this.settings.gain;
    // The sign convention is applied here, once, so the ring holds rates
    // already in the orientation the user asked for and nothing downstream
    // -- bubble, trail, readout, graph, scales -- has to know the setting
    // exists. Which way is "up" is a property of how the headset sits on a
    // particular head, not something the protocol pins down.
    const sx = this.settings.invertX ? -1 : 1;
    const sy = this.settings.invertY ? -1 : 1;
    for (let i = 0; i < t.length; i++) {
      const dt = this.step(t[i]);
      const a = Math.exp(-2 * Math.PI * fc * dt);
      const rx = x[i] * sx;
      const ry = y[i] * sy;
      this.yaw = this.yaw * a + (rx - this.biasX) * dt * gain;
      this.pitch = this.pitch * a + (ry - this.biasY) * dt * gain;
      this.ring.push(t[i], [rx, ry, this.yaw, this.pitch]);
    }
    this.emit();
  }

  /** Stream the estimated head position, at most `sendHz` times a second.
   *
   * The angles and not the rates: a rate is what the hardware already
   * sends, and anything downstream that wanted it can subscribe to
   * `/epoc/gyro/x` directly. The estimate is the thing this widget makes.
   *
   * What goes out is exactly what is drawn -- arbitrary units, after the
   * inversions, the gain and the leaky integrator. Deliberately *not*
   * normalised against the bubble's auto-scale: that scale follows the last
   * few seconds of movement, so the same head position would leave as a
   * different number depending on what happened before it, which is
   * indefensible for something driving a synth. `gain` is the knob for
   * getting the magnitude a receiver wants, and it holds still.
   */
  emit() {
    const s = this.settings;
    if (!s.send) return;
    this.osc.send(performance.now() / 1000, s.sendHz, () => [
      { address: 'gyro/yaw', args: [round3(this.yaw)] },
      { address: 'gyro/pitch', args: [round3(this.pitch)] },
    ]);
  }

  /** Flip one axis's sign convention, the visible history included.
   *
   * Applying it only to samples yet to arrive would leave a step in the
   * middle of the graph at the instant the toggle was clicked, which reads
   * as a movement that never happened. The zero offset is a rate as well, so
   * it flips with the axis; `recompute` then re-derives the angles, which is
   * what carries the flip through to yaw and pitch.
   */
  invert(track) {
    const ring = this.ring;
    for (let i = 0; i < ring.size; i++) ring.setAt(track, i, -ring.valueAt(track, i));
    if (track === RATE_X) this.biasX = -this.biasX;
    else this.biasY = -this.biasY;
    this.recompute();
  }

  /** Sample interval, clamped. A gap -- a dropped burst of datagrams, or a
   * laptop that slept -- must not be integrated as if it were one enormous
   * time step. */
  step(t) {
    const dt = isFinite(this.lastT) ? t - this.lastT : 1 / this.meta.sampleRate;
    this.lastT = t;
    if (!(dt > 0)) return 1 / this.meta.sampleRate;
    return Math.min(dt, 0.1);
  }

  /** Re-derive the whole angle history under the current parameters. */
  recompute() {
    const ring = this.ring;
    const n = ring.size;
    const fc = this.settings.highpassHz;
    const gain = this.settings.gain;
    let yaw = 0;
    let pitch = 0;
    let previous = NaN;
    for (let i = 0; i < n; i++) {
      const t = ring.timeAt(i);
      let dt = isFinite(previous) ? t - previous : 1 / this.meta.sampleRate;
      previous = t;
      if (!(dt > 0)) dt = 1 / this.meta.sampleRate;
      dt = Math.min(dt, 0.1);
      const a = Math.exp(-2 * Math.PI * fc * dt);
      yaw = yaw * a + (ring.valueAt(RATE_X, i) - this.biasX) * dt * gain;
      pitch = pitch * a + (ring.valueAt(RATE_Y, i) - this.biasY) * dt * gain;
      ring.setAt(ANGLE_X, i, yaw);
      ring.setAt(ANGLE_Y, i, pitch);
    }
    this.yaw = yaw;
    this.pitch = pitch;
    this.angleRange.reset(1);
    this.bubbleAngle.reset(1);
  }

  /** Take the recent mean as the resting rate, and put the head back at level.
   *
   * Zeroing is an *event*, not a parameter change, which is what makes it
   * different from `invert` and the two sliders. Those re-derive the visible
   * history through `recompute` so the graph never shows a step the head did
   * not make. Here the step is the whole point -- the user is saying "level
   * is here, now" -- so the integrator state is dropped rather than
   * re-derived, and yaw and pitch read 0 from this instant on.
   *
   * Re-deriving was the bug: `recompute` restarts the integral at the oldest
   * sample in the ring, so it walks the last few seconds of real movement
   * back into the output through the leaky integrator's own memory (a couple
   * of seconds at the default high-pass, more as it is lowered). The button
   * then left the dot somewhere other than the centre, which reads as not
   * having worked at all.
   *
   * The stored angle history is left as it was: it is a record of what was on
   * screen, and rewriting it to zero would claim the head had been level all
   * along. So the graph keeps a step at the moment of the click, and the
   * bubble's trail walks in from wherever the estimate had drifted to --
   * both being the honest picture of what happened.
   */
  zero() {
    const ring = this.ring;
    const n = ring.size;
    if (!n) {
      this.biasX = 0;
      this.biasY = 0;
    } else {
      const from = Math.max(0, ring.lowerBound(ring.latestTime - 1.0));
      let sx = 0;
      let sy = 0;
      let count = 0;
      for (let i = from; i < n; i++) {
        sx += ring.valueAt(RATE_X, i);
        sy += ring.valueAt(RATE_Y, i);
        count++;
      }
      if (count) {
        this.biasX = sx / count;
        this.biasY = sy / count;
      }
    }
    // The two halves of zeroing: the bias above stops the estimate drifting
    // off again, and this drops the state it had accumulated so far. The
    // scales follow the angles, so they are released too -- otherwise the
    // ring stays zoomed out to a deflection that is no longer there.
    this.yaw = 0;
    this.pitch = 0;
    this.angleRange.reset(1);
    this.bubbleAngle.reset(1);
  }

  reset() {
    this.ring.clear();
    this.yaw = 0;
    this.pitch = 0;
    this.lastT = NaN;
    this.displayTime = NaN;
    this.rateRange.reset(1);
    this.angleRange.reset(1);
    this.bubbleRate.reset(1);
    this.bubbleAngle.reset(1);
  }

  // -- parameter bar ------------------------------------------------------

  buildBar(bar) {
    const s = this.settings;
    const save = (changes) => this.store.patch('gyro', changes);

    // Top row: what is on screen and over what span. Bottom row: how the
    // angles are derived, which is set once and then left alone.
    const top = ui.row();
    const bottom = ui.row();
    bar.appendChild(top);
    bar.appendChild(bottom);

    // "rate" and "angle" rather than "x / y" and "yaw / pitch": the flip
    // chips beside them are named for the axes, and two controls on one row
    // reading "x" would each have meant a different thing. The legend under
    // the bubble still spells out which series is which.
    const show = ui.group('show');
    show.appendChild(ui.toggle('rate', s.showRate, (v) => save({ showRate: v }),
      { color: COLOR_RATE }));
    show.appendChild(ui.toggle('angle', s.showAngle, (v) => save({ showAngle: v }),
      { color: COLOR_ANGLE }));
    top.appendChild(show);

    // Which way each axis points is part of what is on screen, so it belongs
    // beside `show` rather than with the integrator's settings -- and the
    // bottom row has two sliders on it already, which at the gyro's default
    // third of the window leaves no room for anything more.
    //
    // One control per axis, not per series: yaw is the integral of x, and
    // flipping a rate without the angle derived from it would be a display
    // that contradicts itself.
    const flip = ui.group('flip');
    const flipX = ui.chip('x', s.invertX, null,
      (v) => { save({ invertX: v }); this.invert(RATE_X); });
    flipX.title = 'Invert x rate and the yaw integrated from it';
    const flipY = ui.chip('y', s.invertY, null,
      (v) => { save({ invertY: v }); this.invert(RATE_Y); });
    flipY.title = 'Invert y rate and the pitch integrated from it';
    flip.appendChild(flipX);
    flip.appendChild(flipY);
    top.appendChild(flip);

    const window = ui.group('window');
    window.appendChild(ui.select(WINDOW_CHOICES.map((v) => [v, `${v}s`]), s.windowSeconds,
      (v) => save({ windowSeconds: Number(v) })));
    top.appendChild(window);

    const range = ui.group('min range');
    range.appendChild(ui.select([[10, '10'], [30, '30'], [60, '60'], [120, '120']],
      s.bubbleRange, (v) => save({ bubbleRange: Number(v) })));
    top.appendChild(range);

    const hp = ui.group('high-pass');
    hp.appendChild(ui.slider({
      min: 0.01, max: 0.5, step: 0.005, value: s.highpassHz,
      // The time constant is the number that actually means something to
      // the user: "how long until it forgets".
      format: (v) => `${v.toFixed(3)} Hz · ${(1 / (2 * Math.PI * v)).toFixed(1)} s`,
    }, (v) => { save({ highpassHz: v }); this.recompute(); }));
    bottom.appendChild(hp);

    const gain = ui.group('gain');
    gain.appendChild(ui.slider({
      min: 0.1, max: 5, step: 0.1, value: s.gain,
      format: (v) => `×${v.toFixed(1)}`,
    }, (v) => { save({ gain: v }); this.recompute(); }));
    gain.appendChild(ui.button('zero', () => this.zero(),
      'Set yaw and pitch back to 0, taking the last second as the resting rate'));
    bottom.appendChild(gain);

    // Beside the two settings that decide what the angles *are*: what goes
    // out is what they produce, so a receiver's idea of "level" changes
    // with them, and having them on one row says so.
    const osc = ui.group('osc');
    const send = ui.toggle('send', s.send, (v) => save({ send: v }), { color: palette.ok });
    send.title = 'Send <prefix>/gyro/yaw and <prefix>/gyro/pitch, in the same '
      + 'arbitrary units shown here';
    osc.appendChild(send);
    const rate = ui.select(SEND_CHOICES.map((v) => [v, `${v} Hz`]), s.sendHz,
      (v) => save({ sendHz: Number(v) }));
    rate.title = 'How often they go out. The top bar’s "out" switch gates them.';
    osc.appendChild(rate);
    bottom.appendChild(osc);
  }

  // -- drawing ------------------------------------------------------------

  render(canvas, { paused }) {
    const fit = fitCanvas(canvas);
    if (!fit) return;
    const { ctx, w, h } = fit;
    const s = this.settings;

    ctx.fillStyle = palette.sunken;
    ctx.fillRect(0, 0, w, h);

    if (!this.ring.size) {
      placeholder(ctx, 'Waiting for gyro data', 0, 0, w, h);
      return;
    }

    const latest = this.ring.latestTime;
    if (isFinite(latest)) {
      if (!isFinite(this.displayTime) || Math.abs(latest - this.displayTime) > 1) {
        this.displayTime = latest;
      } else if (!paused) {
        this.displayTime += (latest - this.displayTime) * 0.3;
      }
    }

    const bubbleH = Math.max(70, Math.min(h * 0.44, 300));
    this.drawBubble(ctx, 0, 0, w, bubbleH);
    hline(ctx, 0, w, bubbleH, palette.line);
    this.drawGraph(ctx, 0, bubbleH, w, h - bubbleH);
  }

  /** Full-scale for each dot: the recent peak, floored so a still headset
   * does not zoom all the way into its own noise. */
  scales() {
    const ring = this.ring;
    const n = ring.size;
    const from = ring.lowerBound(ring.latestTime - 6);
    let rate = 0;
    let angle = 0;
    for (let i = from; i < n; i++) {
      const rx = Math.abs(ring.valueAt(RATE_X, i));
      const ry = Math.abs(ring.valueAt(RATE_Y, i));
      const ax = Math.abs(ring.valueAt(ANGLE_X, i));
      const ay = Math.abs(ring.valueAt(ANGLE_Y, i));
      if (rx > rate) rate = rx;
      if (ry > rate) rate = ry;
      if (ax > angle) angle = ax;
      if (ay > angle) angle = ay;
    }
    const floor = this.settings.bubbleRange;
    return {
      rate: this.bubbleRate.push(Math.max(rate * 1.2, floor)),
      angle: this.bubbleAngle.push(Math.max(angle * 1.2, floor * 0.2)),
    };
  }

  drawBubble(ctx, x, y, w, h) {
    const s = this.settings;
    const legendH = 16;
    const pad = 8;
    const scale = this.scales();
    const n = this.ring.size;

    // The bubble is square and the widget rarely is, so a wide layout has
    // horizontal room to spare, and it is better spent on the numbers than
    // on air. The room the numbers take is reserved on *both* sides, so the
    // bubble stays centred in the widget: a spirit level that sits off to
    // one side -- and slides sideways the moment a readout appears -- is
    // reporting its own layout rather than the position of the head.
    //
    // The bubble therefore gives up a little size to keep the numbers, in
    // the band of widths where both cannot have everything. Only a little:
    // below three quarters of the size it would have had, the numbers are
    // dropped instead.
    const rows = n ? this.readoutRows(n) : [];
    const span = rows.length ? this.readoutSpan(ctx, scale, rows) : null;
    const gutter = span ? pad + span.total : 0;
    const tall = h - pad * 2 - legendH;
    const plain = Math.max(40, Math.min(tall, w - pad * 2));
    const shared = Math.min(tall, w - gutter * 2);
    const hasReadout = !!span && shared >= Math.max(90, plain * 0.75);

    const side = hasReadout ? shared : plain;
    const cx = x + w / 2;
    const cy = y + pad + side / 2;
    const r = side / 2;

    // Rings, crosshair, and the spirit-level frame.
    ctx.save();
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, Math.PI * 2);
    ctx.fillStyle = 'rgba(255,255,255,0.016)';
    ctx.fill();
    for (const f of [1 / 3, 2 / 3, 1]) {
      ctx.beginPath();
      ctx.arc(cx, cy, r * f, 0, Math.PI * 2);
      ctx.strokeStyle = f === 1 ? palette.line : palette.lineSoft;
      ctx.lineWidth = 1;
      ctx.stroke();
    }
    hline(ctx, cx - r, cx + r, cy, palette.lineSoft);
    vline(ctx, cx, cy - r, cy + r, palette.lineSoft);
    ctx.restore();

    const ring = this.ring;
    // Clamp the vector, not each axis: clamping separately lets a corner
    // reach sqrt(2) of the radius and puts the dot outside the ring it is
    // supposed to be bounded by.
    const point = (vx, vy, full) => {
      let ux = vx / full;
      let uy = vy / full;
      const m = Math.sqrt(ux * ux + uy * uy);
      if (m > 1) { ux /= m; uy /= m; }
      return [cx + ux * r, cy - uy * r];
    };

    // The angle dot leaves a short trail, which is what turns two numbers
    // into a sense of which way the head is moving.
    if (s.showAngle && n > 1) {
      const from = ring.lowerBound(ring.latestTime - TRAIL_SECONDS);
      ctx.lineWidth = 1.25;
      ctx.lineJoin = 'round';
      let started = false;
      ctx.beginPath();
      for (let i = from; i < n; i++) {
        const [px, py] = point(ring.valueAt(ANGLE_X, i), ring.valueAt(ANGLE_Y, i), scale.angle);
        if (!started) { ctx.moveTo(px, py); started = true; } else ctx.lineTo(px, py);
      }
      ctx.strokeStyle = 'rgba(167,139,250,0.34)';
      ctx.stroke();
    }

    if (s.showRate && n) {
      const [px, py] = point(ring.valueAt(RATE_X, n - 1), ring.valueAt(RATE_Y, n - 1), scale.rate);
      this.dot(ctx, px, py, 4.5, COLOR_RATE);
    }
    if (s.showAngle && n) {
      const [px, py] = point(ring.valueAt(ANGLE_X, n - 1), ring.valueAt(ANGLE_Y, n - 1), scale.angle);
      this.dot(ctx, px, py, 5.5, COLOR_ANGLE);
    }

    // Full-scale readouts, one per dot, so the rings mean something even
    // though the two series are scaled independently.
    if (w > 150) {
      text(ctx, `±${fmt(scale.rate)}`, cx + r + 6, cy - 7, COLOR_RATE, 'left', 'middle');
      text(ctx, `±${fmt(scale.angle)}`, cx + r + 6, cy + 7, COLOR_ANGLE, 'left', 'middle');
      text(ctx, 'a.u.', cx - r - 6, cy, palette.faint, 'right', 'middle');
    }

    if (hasReadout) this.drawReadout(ctx, cx + r + span.anchor, cy, rows);

    // Whether the stream is running, in the corner the ring cannot reach.
    // Chiefly for the case the switch here is on and the top bar's is not,
    // which is otherwise completely silent.
    const status = this.osc.status(s.send);
    if (status && w > 260) {
      text(ctx, status.text, x + w - 8, y + 9, palette[status.level], 'right', 'middle');
    }

    const legendY = y + h - legendH / 2 - 2;
    const items = [];
    if (s.showRate) items.push(['x / y rate', COLOR_RATE]);
    if (s.showAngle) items.push(['yaw / pitch', COLOR_ANGLE]);
    let lx = cx - (items.length * 84) / 2;
    for (const [name, color] of items) {
      ctx.fillStyle = color;
      ctx.beginPath();
      ctx.arc(lx + 4, legendY, 3.5, 0, Math.PI * 2);
      ctx.fill();
      text(ctx, name, lx + 12, legendY, palette.dim, 'left', 'middle', FONT_LABEL);
      lx += 84;
    }
  }

  /** The rows the readout would show: name, value, colour, decimals. */
  readoutRows(n) {
    const s = this.settings;
    const ring = this.ring;
    const rows = [];
    if (s.showRate) {
      rows.push(['x', ring.valueAt(RATE_X, n - 1), COLOR_RATE, 0]);
      rows.push(['y', ring.valueAt(RATE_Y, n - 1), COLOR_RATE, 0]);
    }
    if (s.showAngle) {
      rows.push(['yaw', ring.valueAt(ANGLE_X, n - 1), COLOR_ANGLE, 1]);
      rows.push(['pitch', ring.valueAt(ANGLE_Y, n - 1), COLOR_ANGLE, 1]);
    }
    return rows;
  }

  /** How much room the numbers need to the right of the ring: the full-scale
   * labels sit closest to it, then the readout's right-aligned names, then
   * the values. `anchor` is where the names end, `total` the whole span.
   *
   * Measured rather than assumed. It decides whether the readout is shown at
   * all, and through the mirrored gutter it also sets the bubble's size --
   * a guessed constant would either waste radius or clip a digit, and which
   * one depends on the font the browser actually picked.
   */
  readoutSpan(ctx, scale, rows) {
    ctx.font = FONT;
    const scaleW = Math.max(ctx.measureText(`±${fmt(scale.rate)}`).width,
      ctx.measureText(`±${fmt(scale.angle)}`).width);
    ctx.font = FONT_LABEL;
    let labelW = 0;
    for (const [name] of rows) labelW = Math.max(labelW, ctx.measureText(name).width);
    ctx.font = VALUE_FONT;
    // A template, not the current values: the layout must not breathe as
    // digits come and go.
    const valueW = ctx.measureText('-000.0').width;
    const anchor = 6 + scaleW + 12 + labelW;
    return { anchor, total: anchor + 10 + valueW };
  }

  /** Current values, large enough to read across a room. */
  drawReadout(ctx, x, cy, rows) {
    if (!rows.length) return;
    const lineH = 20;
    let ry = cy - ((rows.length - 1) * lineH) / 2;
    for (const [name, value, color, decimals] of rows) {
      text(ctx, name, x, ry, palette.faint, 'right', 'middle', FONT_LABEL);
      // A leading sign on every value, so the digits do not shift sideways
      // each time it flips -- at 60 fps that jitter is very distracting.
      const shown = (value >= 0 ? '+' : '-') + Math.abs(value).toFixed(decimals);
      text(ctx, shown, x + 10, ry, color, 'left', 'middle', VALUE_FONT);
      ry += lineH;
    }
  }

  dot(ctx, x, y, r, color) {
    ctx.beginPath();
    ctx.arc(x, y, r + 3, 0, Math.PI * 2);
    ctx.fillStyle = color.replace(')', ' / 18%)').replace('rgb', 'rgba');
    ctx.globalAlpha = 0.22;
    ctx.fillStyle = color;
    ctx.fill();
    ctx.globalAlpha = 1;
    ctx.beginPath();
    ctx.arc(x, y, r, 0, Math.PI * 2);
    ctx.fillStyle = color;
    ctx.fill();
    ctx.lineWidth = 1;
    ctx.strokeStyle = palette.sunken;
    ctx.stroke();
  }

  drawGraph(ctx, x, y, w, h) {
    const s = this.settings;
    if (!s.showRate && !s.showAngle) {
      placeholder(ctx, 'Enable a series', x, y, w, h);
      return;
    }
    const gutL = s.showRate ? 40 : 10;
    const gutR = s.showAngle ? 40 : 10;
    const axBot = 18;
    const plotX = x + gutL;
    const plotW = Math.max(10, w - gutL - gutR);
    const top = y + 8;
    const height = Math.max(10, h - axBot - 12);
    const mid = top + height / 2;
    const window = s.windowSeconds;
    const t1 = this.displayTime;
    const t0 = t1 - window;

    const ring = this.ring;
    const i0 = ring.lowerBound(t0);
    const n = ring.size - i0;
    if (n < 2) {
      placeholder(ctx, 'Waiting for gyro data', x, y, w, h);
      return;
    }

    let ratePeak = 0;
    let anglePeak = 0;
    for (let i = i0; i < ring.size; i++) {
      const rx = Math.abs(ring.valueAt(RATE_X, i));
      const ry = Math.abs(ring.valueAt(RATE_Y, i));
      const ax = Math.abs(ring.valueAt(ANGLE_X, i));
      const ay = Math.abs(ring.valueAt(ANGLE_Y, i));
      if (rx > ratePeak) ratePeak = rx;
      if (ry > ratePeak) ratePeak = ry;
      if (ax > anglePeak) anglePeak = ax;
      if (ay > anglePeak) anglePeak = ay;
    }
    const rateHalf = this.rateRange.push(Math.max(ratePeak * 1.15, 4));
    const angleHalf = this.angleRange.push(Math.max(anglePeak * 1.15, 1));

    // Grid.
    const step = window >= 24 ? 8 : window >= 12 ? 4 : window >= 8 ? 2 : 1;
    for (let sec = step; sec < window; sec += step) {
      vline(ctx, plotX + plotW * (1 - sec / window), top, top + height, palette.lineSoft);
    }
    hline(ctx, plotX, plotX + plotW, mid, palette.lineSoft);

    ctx.save();
    ctx.beginPath();
    ctx.rect(plotX, top, plotW, height);
    ctx.clip();
    // Rate against the left axis, angle against the right: the two have
    // unrelated magnitudes, and a single shared scale would flatten
    // whichever happened to be smaller.
    if (s.showRate) {
      this.series(ctx, RATE_X, i0, n, plotX, plotW, mid, height, t0, window, rateHalf, COLOR_RATE, false);
      this.series(ctx, RATE_Y, i0, n, plotX, plotW, mid, height, t0, window, rateHalf, COLOR_RATE, true);
    }
    if (s.showAngle) {
      this.series(ctx, ANGLE_X, i0, n, plotX, plotW, mid, height, t0, window, angleHalf, COLOR_ANGLE, false);
      this.series(ctx, ANGLE_Y, i0, n, plotX, plotW, mid, height, t0, window, angleHalf, COLOR_ANGLE, true);
    }
    ctx.restore();

    vline(ctx, plotX, top, top + height, palette.line);
    vline(ctx, plotX + plotW, top, top + height, palette.line);
    hline(ctx, plotX, plotX + plotW, top, palette.line);
    hline(ctx, plotX, plotX + plotW, top + height, palette.line);

    if (s.showRate) {
      text(ctx, `+${fmt(rateHalf)}`, plotX - 5, top + 5, COLOR_RATE, 'right', 'middle');
      text(ctx, `-${fmt(rateHalf)}`, plotX - 5, top + height - 5, COLOR_RATE, 'right', 'middle');
    }
    if (s.showAngle) {
      text(ctx, `+${fmt(angleHalf)}`, plotX + plotW + 5, top + 5, COLOR_ANGLE, 'left', 'middle');
      text(ctx, `-${fmt(angleHalf)}`, plotX + plotW + 5, top + height - 5, COLOR_ANGLE, 'left', 'middle');
    }

    const labelY = top + height + axBot / 2;
    for (let sec = 0; sec < window; sec += step) {
      const tx = plotX + plotW * (1 - sec / window);
      vline(ctx, tx, top + height, top + height + 3, palette.line);
      text(ctx, sec === 0 ? 'now' : `-${sec}s`, tx, labelY,
        palette.faint, sec === 0 ? 'right' : 'center', 'middle');
    }
    // Dashing distinguishes the y axis from the x axis within each pair,
    // leaving hue free to distinguish rate from angle. Drawn inside the
    // plot, where it cannot collide with the "now" tick label.
    if (plotW > 150) {
      text(ctx, 'solid x · dashed y', plotX + plotW - 6, top + 8,
        palette.faint, 'right', 'middle');
    }
  }

  series(ctx, track, i0, n, plotX, plotW, mid, height, t0, window, half, color, dashed) {
    const ring = this.ring;
    const data = ring.data[track];
    const times = ring.time;
    const cap = ring.cap;
    const scaleY = (height / 2) / half;
    const scaleX = plotW / window;
    let p = ring.phys(i0);
    ctx.beginPath();
    ctx.setLineDash(dashed ? [3, 3] : []);
    ctx.strokeStyle = color;
    ctx.globalAlpha = dashed ? 0.75 : 1;
    ctx.lineWidth = 1.25;
    ctx.lineJoin = 'round';
    const stride = n > plotW * 2 ? Math.ceil(n / (plotW * 2)) : 1;
    for (let k = 0; k < n; k += stride) {
      const q = (ring.phys(i0 + k));
      const px = plotX + (times[q] - t0) * scaleX;
      const py = mid - data[q] * scaleY;
      if (k === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    }
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.globalAlpha = 1;
  }
}
