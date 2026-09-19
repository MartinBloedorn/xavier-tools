/* The analysis tool: a 2D emotion model, and two cognitive indices.
 *
 * Everything here comes out of the band powers the DAT already sends, so
 * there is no new maths on the acquisition side and nothing to train. The
 * formulae are the ones in view/doc/analysis.md:
 *
 *   valence = ln(alpha F4) - ln(alpha F3)     frontal alpha asymmetry
 *   arousal = ln(beta / alpha)                over the frontal channels
 *   focus   = ln(beta / (alpha + theta))      F3 F4 F7 F8
 *   relax   = ln(alpha O1 + alpha O2)         the eye-close alpha spike
 *
 * Three things are then done to each index, in this order, and the order is
 * most of the design:
 *
 *   1. a one-pole low pass -- raw band powers flutter far too fast to read,
 *      and far too fast to drive anything with;
 *   2. subtraction of a slow baseline -- absolute band power says at least
 *      as much about the skull and the electrode contact as about the mind
 *      underneath, so only the departure from this head's own resting level
 *      means anything;
 *   3. a mapping into the display range: tanh for the two-sided indices, so
 *      an unusually large excursion compresses instead of pinning the dot
 *      into a corner and staying there.
 *
 * The consequence of (2) to know before trusting a reading: a state you
 * hold fades. Concentrate for a minute against a 30 s baseline and focus
 * rises and then settles back toward the middle, because the baseline has
 * followed you there. That is what a trailing baseline is. `calibrate`
 * freezes one instead, which is the mode for when a value has to mean the
 * same thing all evening.
 *
 * None of this is calibrated in any absolute sense -- the ranges are
 * display conventions, exposed rather than baked in for exactly that
 * reason. The dot is an inference, which is why it is the one thing in the
 * viewer drawn in a colour nothing else uses.
 */

import { SampleRing } from '../lib/ring.js';
import * as ui from '../lib/controls.js';
import { OscOut, output, round3 } from '../lib/stream.js';
import {
  FONT, FONT_LABEL, fitCanvas, hline, palette, placeholder, qualityColor,
  roundRect, text, vline,
} from '../lib/plot.js';

const VALUE_FONT = '15px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
const SMALL_FONT = '9px ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif';

const KEYS = ['valence', 'arousal', 'focus', 'relax'];
const PAD_KEYS = new Set(['valence', 'arousal']);
const TRACK = { valence: 0, arousal: 1, focus: 2, relax: 3 };

const FRONTAL = ['F3', 'F4', 'F7', 'F8'];
const AF = ['AF3', 'AF4'];
const OCCIPITAL = ['O1', 'O2'];
/** Right minus left, so a positive result reads as approach/positive mood. */
const ASYMMETRY_PAIRS = [['F4', 'F3'], ['AF4', 'AF3']];

const MAX_HISTORY = 130;        // seconds kept: the longest baseline, plus slack
const UPDATE_HZ = 45;           // band powers can arrive on every SSE tick
const TRAIL_SECONDS = 8;
const CALIBRATE_SECONDS = 10;
/** Band powers are microvolts squared; this keeps ln() finite on a channel
 * reading nothing at all. */
const EPS = 1e-9;

const BASELINE_CHOICES = [10, 30, 60, 120];
const SEND_CHOICES = [5, 10, 20, 30];
const RELAX_CHOICES = [2, 3, 5, 10];

// Canvas geometry. The grid wants to be square and the faders want a fixed
// height, so the split between the two sections is driven from the bottom.
const PAD = 8;
const HEADER_H = 16;
const FOOT_H = 18;
const FADER_H = 34;
const STRIP_MIN = 48;
const GRID_MIN = 150;

const ln = (v) => Math.log(Math.max(v, EPS));
const clamp01 = (v) => (v < 0 ? 0 : v > 1 ? 1 : v);
const signed = (v, digits = 2) => (v >= 0 ? '+' : '-') + Math.abs(v).toFixed(digits);

export class AnalysisWidget {
  static id = 'analysis';
  static title = 'analysis';

  constructor({ meta, store }) {
    this.meta = meta;
    this.store = store;
    this.settings = store.section('analysis');
    this.channelCount = meta.channels.length;

    // Both lookups are by name rather than by position: this widget cares
    // about particular electrodes and particular bands, and an index that
    // silently came to mean something else would be undetectable on screen.
    this.channel = {};
    meta.channels.forEach((name, i) => { this.channel[name.toUpperCase()] = i; });
    this.band = {};
    meta.bands.forEach((band, i) => { this.band[band.name] = i; });

    this.fft = new Array(this.channelCount).fill(null);
    this.quality = new Float32Array(this.channelCount);
    this.hasQuality = false;

    // The ring holds *deviations from baseline, in ln units* -- not the
    // display values. The ranges are a mapping applied when drawing, so
    // moving a range slider re-scales the history already on screen rather
    // than leaving a step in it at the moment of the drag.
    this.ring = new SampleRing(4, Math.ceil(MAX_HISTORY * UPDATE_HZ));

    // Per index: the low-passed raw value, and the baseline it is measured
    // against.
    this.state = {};
    for (const key of KEYS) this.state[key] = { lp: NaN, base: NaN };
    this.dev = null;
    this.hold = null;           // frozen baselines, from `calibrate`
    this.calibration = null;    // { started, elapsed, sums, n } while collecting

    this.lastT = NaN;
    this.lastWall = NaN;
    this.displayTime = NaN;
    this.osc = new OscOut();
    this.calibrateButton = null;
  }

  // -- data ---------------------------------------------------------------

  ingest(msg) {
    if (msg.quality) {
      for (let i = 0; i < this.channelCount; i++) this.quality[i] = msg.quality[i];
      this.hasQuality = true;
    }
    if (!msg.fft) return;       // band powers are this widget's clock
    for (let i = 0; i < this.channelCount; i++) {
      if (msg.fft[i]) this.fft[i] = msg.fft[i];
    }

    // Prefer the stream's own clock, so the history strip is in the same
    // seconds the other widgets plot against. /fft carries no timestamp by
    // the time it reaches the browser, but the raw batch it travels with
    // does; without one, fall back to the wall clock, which is what the
    // axis would have had to mean anyway.
    const wall = performance.now() / 1000;
    let t;
    if (msg.raw && msg.raw.t && msg.raw.t.length) {
      t = msg.raw.t[msg.raw.t.length - 1];
    } else if (isFinite(this.lastT) && isFinite(this.lastWall)) {
      t = this.lastT + Math.min(0.25, Math.max(0, wall - this.lastWall));
    } else {
      t = 0;
    }
    // A gap -- a stalled link, a laptop that slept -- must not go into the
    // filters as one enormous time step.
    const dt = isFinite(this.lastT) ? Math.min(0.25, Math.max(1e-3, t - this.lastT)) : 0;
    this.lastT = t;
    this.lastWall = wall;

    this.update(t, dt);
    this.emit(wall);
  }

  update(t, dt) {
    const s = this.settings;
    const alpha = this.band.alpha;
    const beta = this.band.beta;
    const theta = this.band.theta;
    const frontal = s.padUseAf ? FRONTAL.concat(AF) : FRONTAL;

    const raw = {
      valence: this.asymmetry(alpha, s.padUseAf),
      // One ratio of summed powers, not a mean of per-channel ratios: a
      // single electrode with a near-zero alpha would otherwise dominate
      // the average with a number describing its own contact rather than
      // the head under it.
      arousal: this.logRatio(this.power(frontal, beta), this.power(frontal, alpha)),
      focus: this.logRatio(this.power(FRONTAL, beta),
        this.power(FRONTAL, alpha) + this.power(FRONTAL, theta)),
      // A level rather than a ratio: the eye-close trigger is a spike in
      // absolute occipital alpha.
      relax: ln(this.power(OCCIPITAL, alpha)),
    };
    // The DAT sends all fourteen spectra together, so either every index
    // can be computed or none can -- and during the first FFT interval
    // after a connection, none can.
    for (const key of KEYS) if (!isFinite(raw[key])) return;

    const dev = {};
    for (const key of KEYS) {
      const st = this.state[key];
      const fc = Math.max(0.01, PAD_KEYS.has(key) ? s.padHz : s.cogHz);
      if (!isFinite(st.lp)) st.lp = raw[key];
      else st.lp += (raw[key] - st.lp) * (1 - Math.exp(-2 * Math.PI * fc * dt));

      const tau = Math.max(1, s.baselineSeconds);
      if (!isFinite(st.base)) st.base = st.lp;
      else if (!this.hold) st.base += (st.lp - st.base) * (1 - Math.exp(-dt / tau));

      dev[key] = st.lp - (this.hold ? this.hold[key] : st.base);
    }
    this.dev = dev;
    this.ring.push(t, [dev.valence, dev.arousal, dev.focus, dev.relax]);
    this.collect(t);
  }

  /** Summed band power over a set of electrodes.
   *
   * NaN, not zero, when one of them has no spectrum yet: zero would go
   * through ln() as a very confident and very wrong reading.
   */
  power(names, band) {
    let sum = 0;
    for (const name of names) {
      const ch = this.channel[name];
      const bands = ch === undefined ? null : this.fft[ch];
      if (!bands) return NaN;
      sum += bands[band];
    }
    return sum;
  }

  logRatio(a, b) {
    return ln(a) - ln(b);
  }

  /** Frontal alpha asymmetry, right minus left.
   *
   * With AF3/AF4 in, the two pairs are averaged *in the log domain*. The
   * index is a log ratio, and that is the average which keeps it
   * antisymmetric: swapping the hemispheres negates the result either way,
   * which averaging the raw powers first would not guarantee.
   */
  asymmetry(band, useAf) {
    const pairs = useAf ? ASYMMETRY_PAIRS : ASYMMETRY_PAIRS.slice(0, 1);
    let sum = 0;
    for (const [right, left] of pairs) {
      sum += this.logRatio(this.power([right], band), this.power([left], band));
    }
    return sum / pairs.length;
  }

  /** Collect a fixed baseline over CALIBRATE_SECONDS, then hold it. */
  collect(t) {
    const c = this.calibration;
    if (!c) return;
    if (!isFinite(c.started)) c.started = t;
    for (const key of KEYS) c.sums[key] += this.state[key].lp;
    c.n += 1;
    c.elapsed = t - c.started;
    if (c.elapsed < CALIBRATE_SECONDS) return;
    const held = {};
    for (const key of KEYS) held[key] = c.sums[key] / c.n;
    this.hold = held;
    this.calibration = null;
    // Everything behind this point was measured against a different zero,
    // and drawing them together would show a jump that never happened.
    this.ring.clear();
    this.syncCalibrate();
  }

  // -- display values -----------------------------------------------------

  /** ln-unit deviation -> the two-sided indices, in -1..+1. */
  mapPad(dev) {
    return Math.tanh(dev / Math.max(0.01, this.settings.padRange));
  }

  /** Focus rests at mid-scale: the fader shows departure from this head's
   * own resting level in both directions, not an absolute quantity. */
  mapFocus(dev) {
    return 0.5 + 0.5 * Math.tanh(dev / Math.max(0.01, this.settings.focusRange));
  }

  /** Relax rests at zero and rises: it is a trigger rather than a balance,
   * and its range is set as a multiple of baseline occipital alpha -- which
   * is how the eye-close spike is described, and a number a user can reason
   * about. Linear rather than tanh, so the fader position stays
   * proportional to how far past the trigger the spike went. */
  mapRelax(dev) {
    return clamp01(dev / Math.log(Math.max(1.1, this.settings.relaxRange)));
  }

  values() {
    const d = this.dev;
    if (!d) return null;
    return {
      valence: this.mapPad(d.valence),
      arousal: this.mapPad(d.arousal),
      focus: this.mapFocus(d.focus),
      relax: this.mapRelax(d.relax),
    };
  }

  // -- OSC ----------------------------------------------------------------

  /** Post the current values, at most `sendHz` times a second.
   *
   * Driven from ingest rather than from the frame loop, so the stream does
   * not stop when the widget is closed -- an installation may well want the
   * viewer minding its own business in a background tab. It does stop when
   * the viewer is paused, which is right: the values behind it have stopped
   * too, and repeating a frozen reading at 10 Hz would be a lie.
   */
  emit(wall) {
    const s = this.settings;
    const pad = s.padOn && s.padSend;
    const cog = s.cogOn && s.cogSend;
    if (!pad && !cog) return;
    this.osc.send(wall, s.sendHz, () => {
      const values = this.values();
      if (!values) return null;
      const messages = [];
      if (pad) {
        messages.push({ address: 'pad/valence', args: [round3(values.valence)] });
        messages.push({ address: 'pad/arousal', args: [round3(values.arousal)] });
      }
      if (cog) {
        messages.push({ address: 'cog/focus', args: [round3(values.focus)] });
        messages.push({ address: 'cog/relax', args: [round3(values.relax)] });
      }
      return messages;
    });
  }

  // -- lifecycle ----------------------------------------------------------

  reset() {
    this.ring.clear();
    this.fft.fill(null);
    this.hasQuality = false;
    for (const key of KEYS) { this.state[key].lp = NaN; this.state[key].base = NaN; }
    this.dev = null;
    this.hold = null;
    this.calibration = null;
    this.lastT = NaN;
    this.lastWall = NaN;
    this.displayTime = NaN;
    this.syncCalibrate();
  }

  /** A setting changed what an index *is*, rather than how it is drawn.
   *
   * The filters are re-primed and the history dropped: carrying either
   * across would put a step in the trail at the instant of the click, which
   * reads as a change of mind that never happened.
   */
  invalidate() {
    this.ring.clear();
    for (const key of KEYS) { this.state[key].lp = NaN; this.state[key].base = NaN; }
    this.dev = null;
    this.hold = null;
    this.calibration = null;
    this.syncCalibrate();
  }

  toggleCalibration() {
    if (this.calibration) {
      this.calibration = null;                                  // cancel
    } else if (this.hold) {
      this.hold = null;                                         // back to rolling
      this.ring.clear();
    } else {
      const sums = {};
      for (const key of KEYS) sums[key] = 0;
      this.calibration = { started: NaN, elapsed: 0, sums, n: 0 };
    }
    this.syncCalibrate();
  }

  syncCalibrate() {
    if (!this.calibrateButton) return;
    this.calibrateButton.textContent =
      this.calibration ? 'cancel' : this.hold ? 'release' : 'calibrate';
  }

  // -- parameter bar ------------------------------------------------------

  buildBar(bar) {
    const s = this.settings;
    const save = (changes) => this.store.patch('analysis', changes);

    // Top row the emotion model, bottom row the cognitive pair, as the
    // planning note asks. The two settings belonging to both have to sit on
    // one of them: `baseline` goes up, beside the model whose zero it most
    // visibly defines, and `osc` down, beside the second send switch.
    const top = ui.row();
    const bottom = ui.row();
    bar.appendChild(top);
    bar.appendChild(bottom);

    const emotion = ui.group('emotion');
    const padOn = ui.toggle('on', s.padOn, (v) => save({ padOn: v }), { color: palette.pad });
    padOn.title = 'Compute and draw valence / arousal';
    emotion.appendChild(padOn);
    const padSend = ui.toggle('osc', s.padSend, (v) => save({ padSend: v }), { color: palette.ok });
    padSend.title = 'Send <prefix>/pad/valence and <prefix>/pad/arousal';
    emotion.appendChild(padSend);
    // A chip rather than a third switch: the two switches decide whether
    // the analysis runs at all and whether it leaves the machine, and this
    // only decides what is drawn.
    const padStrip = ui.chip('graph', s.padStrip, palette.pad, (v) => save({ padStrip: v }));
    padStrip.title = 'Plot valence and arousal against time, under the grid';
    emotion.appendChild(padStrip);
    top.appendChild(emotion);

    const pairs = ui.group('pairs');
    const af = ui.chip('+AF3/4', s.padUseAf, palette.pad, (v) => {
      save({ padUseAf: v });
      this.invalidate();
    });
    af.title = 'Average AF3/AF4 into the asymmetry and the beta/alpha ratio, '
      + 'for stability -- restarts both indices';
    pairs.appendChild(af);
    top.appendChild(pairs);

    top.appendChild(this.smoothGroup(s.padHz, (v) => save({ padHz: v })));

    const padRange = ui.group('range');
    padRange.appendChild(ui.slider({
      min: 0.1, max: 2, step: 0.05, value: s.padRange,
      // ln units, because that is what the indices are: full deflection at
      // 0.50 means the right hemisphere's alpha is e^0.5 times the left's.
      format: (v) => `±${v.toFixed(2)} ln`,
    }, (v) => save({ padRange: v })));
    top.appendChild(padRange);

    const baseline = ui.group('baseline');
    const window = ui.select(BASELINE_CHOICES.map((v) => [v, `${v}s`]), s.baselineSeconds,
      (v) => save({ baselineSeconds: Number(v) }));
    window.title = 'How long the rolling baseline remembers -- and the span of '
      + 'the history graphs. Applies to both analyses.';
    baseline.appendChild(window);
    this.calibrateButton = ui.button('calibrate', () => this.toggleCalibration(),
      `Freeze the baseline at the average of the next ${CALIBRATE_SECONDS} seconds, `
      + 'so that a held state stops fading');
    baseline.appendChild(this.calibrateButton);
    this.syncCalibrate();
    top.appendChild(baseline);

    const cognitive = ui.group('cognitive');
    const cogOn = ui.toggle('on', s.cogOn, (v) => save({ cogOn: v }), { color: palette.focus });
    cogOn.title = 'Compute and draw focus / relax';
    cognitive.appendChild(cogOn);
    const cogSend = ui.toggle('osc', s.cogSend, (v) => save({ cogSend: v }), { color: palette.ok });
    cogSend.title = 'Send <prefix>/cog/focus and <prefix>/cog/relax';
    cognitive.appendChild(cogSend);
    const cogStrip = ui.chip('graph', s.cogStrip, palette.focus, (v) => save({ cogStrip: v }));
    cogStrip.title = 'Plot focus and relaxation against time, under the faders';
    cognitive.appendChild(cogStrip);
    bottom.appendChild(cognitive);

    bottom.appendChild(this.smoothGroup(s.cogHz, (v) => save({ cogHz: v })));

    const focus = ui.group('focus');
    focus.appendChild(ui.slider({
      min: 0.1, max: 2, step: 0.05, value: s.focusRange,
      format: (v) => `±${v.toFixed(2)} ln`,
    }, (v) => save({ focusRange: v })));
    bottom.appendChild(focus);

    const relax = ui.group('relax');
    const full = ui.select(RELAX_CHOICES.map((v) => [v, `${v}×`]), s.relaxRange,
      (v) => save({ relaxRange: Number(v) }));
    full.title = 'Occipital alpha, as a multiple of its baseline, that reads as '
      + 'a full fader';
    relax.appendChild(full);
    const trigger = ui.slider({
      min: 0.1, max: 0.95, step: 0.05, value: s.relaxTrigger,
      format: (v) => v.toFixed(2),
    }, (v) => save({ relaxTrigger: v }));
    trigger.title = 'Where the fader reads as "eyes closed"';
    relax.appendChild(trigger);
    bottom.appendChild(relax);

    const osc = ui.group('osc');
    const rate = ui.select(SEND_CHOICES.map((v) => [v, `${v} Hz`]), s.sendHz,
      (v) => save({ sendHz: Number(v) }));
    rate.title = 'How often values go out, for both streams. The top bar’s '
      + '"out" switch gates them.';
    osc.appendChild(rate);
    bottom.appendChild(osc);
  }

  /** The low-pass control, identical for both sections. The time constant
   * is the number that means something here -- how long an index takes to
   * catch up -- so the readout shows both. */
  smoothGroup(value, onChange) {
    const group = ui.group('smooth');
    group.appendChild(ui.slider({
      min: 0.05, max: 2, step: 0.05, value,
      format: (v) => `${v.toFixed(2)} Hz · ${(1 / (2 * Math.PI * v)).toFixed(1)} s`,
    }, onChange));
    return group;
  }

  // -- drawing ------------------------------------------------------------

  render(canvas, { paused }) {
    const fit = fitCanvas(canvas);
    if (!fit) return;
    const { ctx, w, h } = fit;

    ctx.fillStyle = palette.sunken;
    ctx.fillRect(0, 0, w, h);

    if (!this.ring.size) {
      placeholder(ctx, 'Waiting for band powers', 0, 0, w, h);
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

    // Heights are settled from the bottom up: the cognitive section's
    // contents are all of fixed size, so it is measured first and the
    // emotion model takes what is left -- its grid is the one elastic thing
    // on screen.
    //
    // A section that is switched off keeps its header and a note saying so,
    // and gives the rest of its height to the other one. Holding space for
    // a disabled analysis would shrink the one being looked at in order to
    // display the words "analysis off" in a larger font.
    const s = this.settings;
    const folded = HEADER_H + 28;
    const faders = HEADER_H + FADER_H * 2 + PAD;
    const strips = this.stripHeights(h);
    let cogH;
    if (!s.cogOn) {
      cogH = folded;
    } else if (!s.padOn) {
      // Nothing above worth leaving room for: take the widget.
      cogH = Math.max(faders + strips.cog, h - folded);
    } else {
      cogH = faders + strips.cog;
    }
    cogH = Math.min(cogH, Math.max(folded, h - folded));

    this.drawEmotion(ctx, 0, 0, w, h - cogH, strips.pad);
    hline(ctx, 0, w, h - cogH, palette.line);
    this.drawCognitive(ctx, 0, h - cogH, w, cogH, strips.cog);
  }

  /** How tall each history strip may be.
   *
   * They are the first thing to go when the widget is short: a fader and a
   * dot that read correctly matter more than the record behind them, and
   * the grid has a size below which it stops being a grid.
   *
   * Both or neither, when both are asked for. One strip quietly missing
   * would read as a fault in whichever index lost the draw, and the two are
   * being compared -- which is most of the reason to have them.
   */
  stripHeights(h) {
    const s = this.settings;
    const folded = HEADER_H + 28;
    const padFixed = s.padOn ? HEADER_H + FOOT_H + GRID_MIN + 8 : folded;
    const cogFixed = s.cogOn ? HEADER_H + FADER_H * 2 + PAD : folded;
    const wanted = [];
    if (s.padOn && s.padStrip) wanted.push('pad');
    if (s.cogOn && s.cogStrip) wanted.push('cog');

    const out = { pad: 0, cog: 0 };
    if (!wanted.length) return out;
    const share = (h - padFixed - cogFixed) / wanted.length - 6;
    if (share < STRIP_MIN) return out;
    // A strip stops growing once it is tall enough to read a shape out of,
    // and the grid keeps the rest. Unless there is no grid to keep it: a
    // folded section has no use for the height, and the faders it belongs
    // to cannot use it either, so the strip may as well.
    const cap = { pad: 96, cog: s.padOn ? 96 : Infinity };
    for (const key of wanted) out[key] = Math.min(share, cap[key]);
    return out;
  }

  // -- the emotion model --------------------------------------------------

  drawEmotion(ctx, x, y, w, h, stripH) {
    const s = this.settings;
    this.header(ctx, x, y, w, 'emotion', 'valence · arousal', this.padStatus());

    const top = y + HEADER_H + 4;
    const avail = Math.max(40, h - HEADER_H - FOOT_H - 8 - stripH);
    if (!s.padOn) {
      placeholder(ctx, 'emotion analysis off', x, top, w, avail);
      return;
    }
    const values = this.values();
    if (!values) {
      placeholder(ctx, 'Waiting for band powers', x, top, w, avail);
      return;
    }

    // The same bargain as the gyro's bubble: the square rarely fills a
    // widget's width, and the room left over is better spent on the numbers
    // than on air. It is reserved on *both* sides so the grid stays
    // centred -- a model that slid sideways the moment a readout appeared
    // would be reporting its own layout rather than a state of mind.
    const rows = [['valence', values.valence], ['arousal', values.arousal]];
    const span = this.readoutSpan(ctx, rows);
    const gutter = PAD + span.total;
    const plain = Math.max(60, Math.min(avail, w - PAD * 2));
    const shared = Math.min(avail, w - gutter * 2);
    const hasReadout = shared >= Math.max(110, plain * 0.75);
    const side = hasReadout ? shared : plain;

    const cx = x + w / 2;
    const gx = cx - side / 2;
    const gy = top + (avail - side) / 2;

    this.drawGrid(ctx, gx, gy, side);
    this.drawTrail(ctx, gx, gy, side);
    const [px, py] = this.gridPoint(gx, gy, side, values.valence, values.arousal);
    this.dot(ctx, px, py, 5.5, palette.pad);

    if (hasReadout) this.drawReadout(ctx, cx + side / 2 + span.anchor, gy + side / 2, rows);

    // The grid says where the state is and the trail says which way it is
    // going, but neither survives more than a few seconds. Unfolding the
    // same two indices against time is what makes a slow drift -- exactly
    // what a trailing baseline produces -- visible as a drift rather than
    // as a dot that happens to be over there now.
    if (stripH) {
      this.drawStrip(ctx, x + PAD, top + avail + 2, w - PAD * 2, stripH - 6, [
        [TRACK.valence, (v) => (this.mapPad(v) + 1) / 2, palette.pad, false],
        [TRACK.arousal, (v) => (this.mapPad(v) + 1) / 2, palette.pad, true],
      ], 'solid valence · dashed arousal');
    }

    // The reminder the planning note asks for: these are the electrodes the
    // model is built on. With their contact quality, because a reading
    // taken from one that is not seated is not a reading at all.
    const names = s.padUseAf ? FRONTAL.concat(AF) : FRONTAL;
    const namesW = this.electrodeSpan(ctx, names);
    const footY = y + h - FOOT_H / 2 - 2;
    if (hasReadout) {
      this.drawElectrodes(ctx, cx - namesW / 2, footY, names);
      return;
    }
    // Too narrow for numbers beside the grid: they drop into the footer and
    // the reminder moves to the other end of it. Narrower still and the
    // reminder is what stays -- the dot is already a reading of the state,
    // whereas an electrode nobody seated is invisible without it.
    ctx.font = FONT;
    const compact = `valence ${signed(values.valence)}   arousal ${signed(values.arousal)}`;
    const compactW = ctx.measureText(compact).width;
    if (compactW + namesW + 20 <= w - PAD * 2) {
      text(ctx, compact, x + PAD, footY, palette.pad, 'left', 'middle', FONT);
      this.drawElectrodes(ctx, x + w - PAD - namesW, footY, names);
    } else {
      this.drawElectrodes(ctx, cx - namesW / 2, footY, names);
    }
  }

  drawGrid(ctx, gx, gy, side) {
    ctx.fillStyle = 'rgba(255,255,255,0.016)';
    ctx.fillRect(gx, gy, side, side);
    for (let i = 1; i < 4; i++) {
      const at = (side * i) / 4;
      vline(ctx, gx + at, gy, gy + side, i === 2 ? palette.line : palette.lineSoft);
      hline(ctx, gx, gx + side, gy + at, i === 2 ? palette.line : palette.lineSoft);
    }
    hline(ctx, gx, gx + side, gy, palette.line);
    hline(ctx, gx, gx + side, gy + side, palette.line);
    vline(ctx, gx, gy, gy + side, palette.line);
    vline(ctx, gx + side, gy, gy + side, palette.line);

    // Russell's quadrants, which are the reason for plotting the two
    // indices against each other rather than as two more faders.
    if (side >= 150) {
      const inset = 8;
      const corners = [
        ['stressed', gx + inset, gy + inset, 'left'],
        ['excited', gx + side - inset, gy + inset, 'right'],
        ['bored', gx + inset, gy + side - inset, 'left'],
        ['calm', gx + side - inset, gy + side - inset, 'right'],
      ];
      for (const [name, tx, ty, align] of corners) {
        text(ctx, name, tx, ty, palette.faint, align, 'middle', SMALL_FONT);
      }
    }
    if (side >= 110) {
      text(ctx, 'valence +', gx + side - 5, gy + side / 2 - 7,
        palette.faint, 'right', 'middle', SMALL_FONT);
      text(ctx, 'arousal +', gx + side / 2 + 5, gy + 9,
        palette.faint, 'left', 'middle', SMALL_FONT);
    }
  }

  gridPoint(gx, gy, side, valence, arousal) {
    return [
      gx + clamp01((valence + 1) / 2) * side,
      gy + side - clamp01((arousal + 1) / 2) * side,
    ];
  }

  /** A short trail behind the dot. Two numbers say where the state is; the
   * trail is what says which way it is going. */
  drawTrail(ctx, gx, gy, side) {
    const ring = this.ring;
    const n = ring.size;
    if (n < 2) return;
    const from = ring.lowerBound(ring.latestTime - TRAIL_SECONDS);
    if (n - from < 2) return;
    ctx.save();
    ctx.beginPath();
    ctx.rect(gx, gy, side, side);
    ctx.clip();
    ctx.beginPath();
    for (let i = from; i < n; i++) {
      const [px, py] = this.gridPoint(gx, gy, side,
        this.mapPad(ring.valueAt(TRACK.valence, i)),
        this.mapPad(ring.valueAt(TRACK.arousal, i)));
      if (i === from) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    }
    ctx.strokeStyle = palette.pad;
    ctx.globalAlpha = 0.3;
    ctx.lineWidth = 1.25;
    ctx.lineJoin = 'round';
    ctx.stroke();
    ctx.globalAlpha = 1;
    ctx.restore();
  }

  /** How much room the numbers need to the right of the grid. `anchor` is
   * where the names end, `total` the whole span -- measured rather than
   * guessed, because through the mirrored gutter it also sets the grid's
   * size, and a constant would either waste width or clip a digit depending
   * on the font the browser picked. */
  readoutSpan(ctx, rows) {
    ctx.font = FONT_LABEL;
    let labelW = 0;
    for (const [name] of rows) labelW = Math.max(labelW, ctx.measureText(name).width);
    ctx.font = VALUE_FONT;
    // A template, not the current values: the layout must not breathe as
    // digits come and go.
    const valueW = ctx.measureText('-0.00').width;
    const anchor = 10 + labelW;
    return { anchor, total: anchor + 10 + valueW };
  }

  drawReadout(ctx, x, cy, rows) {
    const lineH = 20;
    let ry = cy - ((rows.length - 1) * lineH) / 2;
    for (const [name, value] of rows) {
      text(ctx, name, x, ry, palette.faint, 'right', 'middle', FONT_LABEL);
      // A leading sign on every value, so the digits do not shift sideways
      // each time it flips.
      text(ctx, signed(value), x + 10, ry, palette.pad, 'left', 'middle', VALUE_FONT);
      ry += lineH;
    }
  }

  // -- the cognitive pair -------------------------------------------------

  drawCognitive(ctx, x, y, w, h, strip) {
    const s = this.settings;
    // The electrodes go in the header rather than beside each fader: four
    // names and a pair do not fit in the column a fader's name claims, and
    // widening that column would eat into the track at the widths this
    // widget actually gets.
    this.header(ctx, x, y, w, 'cognitive', '', this.cogStatus(),
      s.cogOn ? FRONTAL.concat(['·'], OCCIPITAL) : null);

    let top = y + HEADER_H;
    if (!s.cogOn) {
      placeholder(ctx, 'cognitive analysis off', x, top, w, h - HEADER_H);
      return;
    }
    const values = this.values();
    if (!values) {
      placeholder(ctx, 'Waiting for band powers', x, top, w, h - HEADER_H);
      return;
    }

    // With the emotion model folded away this section holds the whole
    // widget, and its contents are all of fixed height. Centre them: two
    // faders pinned to the top of an empty field look like the start of a
    // list that failed to load.
    const content = FADER_H * 2 + (strip ? strip + 2 : 0);
    top += Math.max(0, (h - HEADER_H - PAD - content) / 2);

    this.drawFader(ctx, x, top, w, 'focus', values.focus, palette.focus,
      { rest: 0.5 });
    this.drawFader(ctx, x, top + FADER_H, w, 'relax', values.relax, palette.relax,
      { trigger: s.relaxTrigger, caption: 'eyes closed' });
    if (strip) {
      this.drawStrip(ctx, x + PAD, top + FADER_H * 2 + 2, w - PAD * 2, strip - 6, [
        [TRACK.focus, (v) => this.mapFocus(v), palette.focus, false],
        [TRACK.relax, (v) => this.mapRelax(v), palette.relax, false],
      ], null);
    }
  }

  /** One horizontal fader. Two of these rather than a second grid: focus
   * and relaxation come from different lobes and move independently, and
   * plotting them as a pair of coordinates would imply a relationship the
   * maths does not have.
   */
  drawFader(ctx, x, y, w, name, value, color, opts) {
    ctx.font = FONT_LABEL;
    const labelW = Math.max(ctx.measureText('focus').width, ctx.measureText('relax').width);
    ctx.font = VALUE_FONT;
    const valueW = ctx.measureText('0.00').width;

    const trackX = x + PAD + labelW + 10;
    const trackW = Math.max(24, w - PAD * 2 - labelW - valueW - 22);
    const trackH = 13;
    const ty = y + (FADER_H - trackH) / 2;
    const triggered = opts.trigger !== undefined && value >= opts.trigger;

    text(ctx, name, x + PAD, ty + trackH / 2, triggered ? color : palette.dim,
      'left', 'middle', FONT_LABEL);

    ctx.fillStyle = palette.lineSoft;
    roundRect(ctx, trackX, ty, trackW, trackH, 3);
    ctx.fill();

    const fillW = Math.max(2, trackW * clamp01(value));
    ctx.save();
    roundRect(ctx, trackX, ty, trackW, trackH, 3);
    ctx.clip();
    ctx.globalAlpha = triggered ? 0.95 : 0.7;
    ctx.fillStyle = color;
    ctx.fillRect(trackX, ty, fillW, trackH);
    ctx.globalAlpha = 1;
    if (triggered && opts.caption && trackW > 150) {
      // Dark on the fill, the fader's own colour on the bare groove: the
      // caption appears at the trigger, which is well short of the right
      // end, so it may sit on either.
      ctx.font = SMALL_FONT;
      const covered = fillW > trackW - ctx.measureText(opts.caption).width - 12;
      text(ctx, opts.caption, trackX + trackW - 6, ty + trackH / 2,
        covered ? palette.sunken : color, 'right', 'middle', SMALL_FONT);
    }
    ctx.restore();
    // A bright leading edge: the fill alone reads well across a room but
    // poorly to a couple of percent, and both are wanted.
    vline(ctx, trackX + fillW, ty, ty + trackH, color, 1.5);

    // Where the index sits when nothing is happening -- mid-scale for
    // focus, the trigger for relax. Without it, a fader resting at half is
    // indistinguishable from one saying "moderately focused".
    if (opts.rest !== undefined) {
      vline(ctx, trackX + trackW * opts.rest, ty - 3, ty + trackH + 3, palette.faint);
    }
    if (opts.trigger !== undefined) {
      vline(ctx, trackX + trackW * clamp01(opts.trigger), ty - 3, ty + trackH + 3,
        triggered ? color : palette.faint);
    }

    text(ctx, value.toFixed(2), x + w - PAD, ty + trackH / 2, color,
      'right', 'middle', VALUE_FONT);
  }

  /** A pair of indices over the baseline window.
   *
   * The dot and the faders say where the state is now; neither can say
   * whether it got there gradually or in a jump, which for a trigger is the
   * whole question. The span is the baseline window on purpose: that is
   * exactly the stretch of time the current zero was computed from, so the
   * mid-line is both "no deviation" and "the average of what is drawn".
   *
   * `traces` are `[track, map, colour, dashed]`, where `map` takes a stored
   * ln-unit deviation to a fraction of the strip's height -- the same
   * mapping the section above uses, so a trace and the thing it is a
   * history of cannot disagree.
   */
  drawStrip(ctx, x, y, w, h, traces, caption) {
    const window = Math.max(5, this.settings.baselineSeconds);
    const t1 = this.displayTime;
    const t0 = t1 - window;
    const ring = this.ring;
    const i0 = ring.lowerBound(t0);
    const n = ring.size - i0;

    hline(ctx, x, x + w, y, palette.lineSoft);
    hline(ctx, x, x + w, y + h, palette.lineSoft);
    hline(ctx, x, x + w, y + h / 2, palette.lineSoft);
    if (n < 2) return;

    ctx.save();
    ctx.beginPath();
    ctx.rect(x, y, w, h);
    ctx.clip();
    for (const [track, map, color, dashed] of traces) {
      this.stripTrace(ctx, track, map, i0, n, x, y, w, h, t0, window, color, dashed);
    }
    ctx.restore();

    if (w > 140) {
      text(ctx, `-${window}s`, x + 3, y + 7, palette.faint, 'left', 'middle');
      text(ctx, 'now', x + w - 3, y + 7, palette.faint, 'right', 'middle');
    }
    // Two traces of one colour need saying which is which. Dashing rather
    // than a second hue, as in the gyro: the emotion model has one colour
    // in this viewer and spending another on half of it would break the
    // association with the dot.
    //
    // On a scrim, because there is no corner of a strip this full a trace
    // cannot wander into, and a caption that has to be deciphered against
    // the data it explains is worse than none.
    if (caption && w > 260) {
      ctx.font = FONT;
      const width = ctx.measureText(caption).width;
      const cy = y + h - 7;
      ctx.globalAlpha = 0.82;
      ctx.fillStyle = palette.sunken;
      ctx.fillRect(x + w - 6 - width, cy - 6, width + 6, 12);
      ctx.globalAlpha = 1;
      text(ctx, caption, x + w - 3, cy, palette.faint, 'right', 'middle');
    }
  }

  stripTrace(ctx, track, map, i0, n, x, y, w, h, t0, window, color, dashed) {
    const ring = this.ring;
    const times = ring.time;
    const data = ring.data[track];
    const scaleX = w / window;
    const stride = n > w * 2 ? Math.ceil(n / (w * 2)) : 1;
    ctx.beginPath();
    ctx.setLineDash(dashed ? [3, 3] : []);
    ctx.globalAlpha = dashed ? 0.75 : 1;
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.25;
    ctx.lineJoin = 'round';
    for (let k = 0; k < n; k += stride) {
      const p = ring.phys(i0 + k);
      const px = x + (times[p] - t0) * scaleX;
      const py = y + h - clamp01(map(data[p])) * h;
      if (k === 0) ctx.moveTo(px, py);
      else ctx.lineTo(px, py);
    }
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.globalAlpha = 1;
  }

  // -- shared furniture ---------------------------------------------------

  /** A section's title line. Its right-hand end carries whichever of three
   * things most needs saying: an OSC problem, a calibration in progress, or
   * what the baseline is currently doing. */
  header(ctx, x, y, w, title, subtitle, status, electrodes) {
    const cy = y + HEADER_H / 2 + 1;
    text(ctx, title, x + PAD, cy, palette.dim, 'left', 'middle', FONT_LABEL);
    ctx.font = FONT_LABEL;
    let at = x + PAD + ctx.measureText(title).width + 8;
    if (subtitle) {
      text(ctx, subtitle, at, cy, palette.faint, 'left', 'middle', SMALL_FONT);
      ctx.font = SMALL_FONT;
      at += ctx.measureText(subtitle).width + 8;
    }
    let right = x + w - PAD;
    if (status) {
      text(ctx, status.text, right, cy, status.color, 'right', 'middle', SMALL_FONT);
      ctx.font = SMALL_FONT;
      right -= ctx.measureText(status.text).width + 10;
    }
    // The reminder is the first thing on this line to go when it is
    // crowded: it is the only part that does not change second to second.
    if (electrodes && at + this.electrodeSpan(ctx, electrodes, true) <= right) {
      this.drawElectrodes(ctx, at, cy, electrodes, true);
    }
  }

  /** What to say about the emotion model's state, in priority order. */
  padStatus() {
    const s = this.settings;
    if (s.padOn && s.padSend && !output.enabled) {
      return { text: 'osc output off', color: palette.warn };
    }
    if (this.osc.error) return { text: `osc: ${this.osc.error}`, color: palette.bad };
    if (this.calibration) {
      const elapsed = Math.max(0, this.calibration.elapsed || 0);
      return {
        text: `calibrating ${elapsed.toFixed(1)} / ${CALIBRATE_SECONDS} s`,
        color: palette.warn,
      };
    }
    if (this.hold) return { text: 'baseline held', color: palette.dim };
    return { text: `baseline ${s.baselineSeconds}s rolling`, color: palette.faint };
  }

  /** The output line. It lives on the cognitive header for both streams --
   * they share one rate limiter, so there is one thing to report, and the
   * emotion header's slot is already spoken for by the baseline. */
  cogStatus() {
    const s = this.settings;
    const sending = (s.padOn && s.padSend) || (s.cogOn && s.cogSend);
    const status = this.osc.status(sending);
    return status && { text: status.text, color: palette[status.level] };
  }

  /** Width an electrode reminder would take, so the caller can centre it or
   * decide it does not fit. */
  electrodeSpan(ctx, names, small = false) {
    ctx.font = small ? SMALL_FONT : FONT;
    let width = 0;
    for (const name of names) {
      const isChannel = this.channel[name] !== undefined;
      width += (isChannel && this.hasQuality ? 12 : 6) + ctx.measureText(name).width + 5;
    }
    return width;
  }

  /** The electrodes an index is built on, each with its contact quality.
   * Discreet on purpose -- it is a reminder, not a reading -- but coloured,
   * because a model fed by an electrode that is not seated is the failure
   * this widget is most likely to suffer and least likely to show. */
  drawElectrodes(ctx, x, cy, names, small = false) {
    const font = small ? SMALL_FONT : FONT;
    let at = x;
    for (const name of names) {
      // A list may carry a separator between two groups of electrodes; it
      // is not one, and must not be given a contact dot.
      const ch = this.channel[name];
      if (ch !== undefined && this.hasQuality) {
        ctx.beginPath();
        ctx.arc(at + 3, cy, 3, 0, Math.PI * 2);
        ctx.fillStyle = qualityColor(this.quality[ch], this.meta.qualityFullScale);
        ctx.fill();
        at += 12;
      } else {
        at += 6;
      }
      text(ctx, name, at - 3, cy, palette.faint, 'left', 'middle', font);
      ctx.font = font;
      at += ctx.measureText(name).width + 5;
    }
  }

  dot(ctx, x, y, r, color) {
    ctx.globalAlpha = 0.22;
    ctx.beginPath();
    ctx.arc(x, y, r + 4, 0, Math.PI * 2);
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
}
