/* The data viewer: raw, quality and fft, fourteen channels at once.
 *
 * Each channel is one row. The scrolling trace reads against the left axis
 * (value) and the bottom axis (time); the band-power bars read against the
 * right axis (power) and the top axis (frequency). Both live in the same
 * row because they describe the same electrode, and separating them would
 * halve the vertical space each gets.
 *
 * The two are told apart by weight rather than by hue alone: the trace is
 * drawn at full saturation over the bars, which are a wash of amber behind
 * it.
 */

import { SampleRing } from '../lib/ring.js';
import * as ui from '../lib/controls.js';
import {
  FONT, FONT_LABEL, Smoothed, channelColor, fmt, fitCanvas, hline,
  niceTicks, palette, placeholder, qualityColor, roundRect, text, vline,
} from '../lib/plot.js';

const MAX_WINDOW = 30;            // seconds; sets the ring capacity
const WINDOW_CHOICES = [2, 4, 8, 16, 30];
const SCALE_CHOICES = ['auto', 25, 50, 100, 200, 500, 1000];
const FFT_DECADES = 3;            // log range below the peak

export class DataWidget {
  static id = 'data';
  static title = 'data';

  constructor({ meta, store }) {
    this.meta = meta;
    this.store = store;
    this.settings = store.section('data');
    this.channelCount = meta.channels.length;

    this.ring = new SampleRing(this.channelCount, Math.ceil(MAX_WINDOW * meta.sampleRate) + 8);
    this.quality = new Float32Array(this.channelCount);
    this.hasQuality = false;
    this.fft = new Array(this.channelCount).fill(null);
    this.hasFft = false;

    this.colors = [];
    for (let i = 0; i < this.channelCount; i++) {
      this.colors.push(channelColor(i, this.channelCount));
    }

    // One smoother per channel for the value range, plus one for the shared
    // band-power range. Without these the axes twitch every frame.
    this.vRange = [];
    this.vCenter = new Float64Array(this.channelCount);
    this.vCentered = new Uint8Array(this.channelCount);
    for (let i = 0; i < this.channelCount; i++) this.vRange.push(new Smoothed(1));
    this.fRange = [];
    for (let i = 0; i < this.channelCount; i++) this.fRange.push(new Smoothed(1));
    this.fShared = new Smoothed(1);

    // Scratch for min/max decimation, grown on demand.
    this._colMin = new Float32Array(0);
    this._colMax = new Float32Array(0);
    this._colHit = new Uint8Array(0);

    this.displayTime = NaN;
    this.chips = [];
  }

  // -- data ---------------------------------------------------------------

  ingest(msg) {
    const raw = msg.raw;
    if (raw) {
      const times = raw.t;
      const frames = raw.v;
      for (let i = 0; i < times.length; i++) this.ring.push(times[i], frames[i]);
    }
    if (msg.quality) {
      for (let i = 0; i < this.channelCount; i++) this.quality[i] = msg.quality[i];
      this.hasQuality = true;
    }
    if (msg.fft) {
      for (let i = 0; i < this.channelCount; i++) {
        if (msg.fft[i]) this.fft[i] = msg.fft[i];
      }
      this.hasFft = this.fft.some((v) => v !== null);
    }
  }

  reset() {
    this.ring.clear();
    this.fft.fill(null);
    this.hasFft = false;
    this.hasQuality = false;
    this.displayTime = NaN;
    for (const s of this.vRange) s.reset(1);
    for (const s of this.fRange) s.reset(1);
    this.fShared.reset(1);
    this.vCentered.fill(0);
  }

  // -- parameter bar ------------------------------------------------------

  buildBar(bar) {
    const s = this.settings;
    const save = (changes) => this.store.patch('data', changes);

    // Top row is channel selection alone -- fourteen chips plus all/none is
    // most of the bar's width, and mixing anything else in meant the row
    // scrolled before the widget was even narrow.
    const top = ui.row();
    const bottom = ui.row();
    bar.appendChild(top);
    bar.appendChild(bottom);

    const channels = ui.group('ch', { scroll: true });
    this.chips = [];
    for (let i = 0; i < this.channelCount; i++) {
      const chip = ui.chip(this.meta.channels[i], s.channels[i], this.colors[i],
        (on, event) => {
          // Alt-click solos a channel: with fourteen toggles, "just this
          // one" is otherwise fourteen clicks.
          if (event.altKey) {
            const solo = s.channels.map((_, k) => k === i);
            save({ channels: solo });
            this.syncChips();
            return;
          }
          const next = s.channels.slice();
          next[i] = on;
          save({ channels: next });
        });
      chip.title = `${this.meta.channels[i]} -- alt-click to solo`;
      this.chips.push(chip);
      channels.appendChild(chip);
    }
    top.appendChild(channels);

    const sel = ui.group('');
    sel.appendChild(ui.button('all', () => {
      save({ channels: new Array(this.channelCount).fill(true) });
      this.syncChips();
    }, 'Enable every channel'));
    sel.appendChild(ui.button('none', () => {
      save({ channels: new Array(this.channelCount).fill(false) });
      this.syncChips();
    }, 'Disable every channel'));
    top.appendChild(sel);

    const view = ui.group('view');
    view.appendChild(ui.toggle('raw', s.showRaw, (v) => save({ showRaw: v }),
      { color: palette.trace }));
    view.appendChild(ui.toggle('fft', s.showFft, (v) => save({ showFft: v }),
      { color: palette.bars }));
    view.appendChild(ui.toggle('quality', s.showQuality, (v) => save({ showQuality: v }),
      { color: palette.ok }));
    bottom.appendChild(view);

    const scale = ui.group('scale');
    scale.appendChild(ui.segmented(
      [['uv', 'µV', 'Approximate microvolts (0.51 µV/count, nominal)'],
       ['counts', 'raw', 'Raw 14-bit ADC counts']],
      s.units, (v) => { save({ units: v }); this.rescale(); },
    ));
    scale.appendChild(ui.select(
      SCALE_CHOICES.map((v) => [v, v === 'auto' ? 'auto' : `±${v}`]),
      s.scale, (v) => save({ scale: v === 'auto' ? 'auto' : Number(v) }),
    ));
    // Phrased as AC coupling rather than "remove DC": on a scope that is
    // the same switch, and the LED then lights when the thing named is
    // happening rather than when it is not.
    const ac = ui.toggle('ac', s.removeDc, (v) => { save({ removeDc: v }); this.rescale(); },
      { color: palette.warn });
    ac.title = 'AC coupling: subtract the window mean, so the rhythms are '
      + 'visible above the ~8400-count DC offset';
    scale.appendChild(ac);
    bottom.appendChild(scale);

    const time = ui.group('window');
    time.appendChild(ui.select(
      WINDOW_CHOICES.map((v) => [v, `${v}s`]), s.windowSeconds,
      (v) => save({ windowSeconds: Number(v) }),
    ));
    bottom.appendChild(time);

    const fft = ui.group('fft');
    fft.appendChild(ui.segmented([['log', 'log'], ['linear', 'lin']], s.fftScale,
      (v) => save({ fftScale: v })));
    fft.appendChild(ui.toggle('shared', s.fftShared, (v) => save({ fftShared: v }),
      { color: palette.bars }));
    fft.appendChild(ui.select([[45, '45 Hz'], [30, '30 Hz'], [64, '64 Hz']], s.fftMaxHz,
      (v) => save({ fftMaxHz: Number(v) })));
    bottom.appendChild(fft);
  }

  syncChips() {
    for (let i = 0; i < this.chips.length; i++) this.chips[i].set(this.settings.channels[i]);
  }

  /** Units or DC coupling changed: the smoothed ranges are in the old
   * domain and would take a second to converge. Drop them instead. */
  rescale() {
    for (const s of this.vRange) s.reset(1);
    this.vCentered.fill(0);
  }

  // -- drawing ------------------------------------------------------------

  render(canvas, { paused }) {
    const fit = fitCanvas(canvas);
    if (!fit) return;
    const { ctx, w, h } = fit;
    const s = this.settings;

    ctx.fillStyle = palette.sunken;
    ctx.fillRect(0, 0, w, h);

    const enabled = [];
    for (let i = 0; i < this.channelCount; i++) if (s.channels[i]) enabled.push(i);

    if (!enabled.length) {
      placeholder(ctx, 'No channels enabled', 0, 0, w, h);
      return;
    }
    if (!s.showRaw && !s.showFft) {
      placeholder(ctx, 'Enable the raw or fft view', 0, 0, w, h);
      return;
    }
    if (!this.ring.size && !this.hasFft) {
      placeholder(ctx, 'Waiting for data', 0, 0, w, h);
      return;
    }

    // Ease the right-hand edge toward the newest sample. Batches arrive at
    // 40 Hz and the display runs at 60, so following the raw value directly
    // makes the trace advance in visible steps.
    const latest = this.ring.latestTime;
    if (isFinite(latest)) {
      if (!isFinite(this.displayTime) || Math.abs(latest - this.displayTime) > 1) {
        this.displayTime = latest;
      } else if (!paused) {
        this.displayTime += (latest - this.displayTime) * 0.3;
      }
    }

    const showFft = s.showFft && this.hasFft;
    const showRaw = s.showRaw;
    const gutL = 46;
    const gutR = showFft ? 44 : 10;
    const axTop = showFft ? 22 : 6;
    const axBot = showRaw ? 20 : 6;
    const plotX = gutL;
    const plotW = Math.max(10, w - gutL - gutR);
    const areaY = axTop;
    const areaH = Math.max(10, h - axTop - axBot);
    const rowH = areaH / enabled.length;
    const dense = rowH < 26;          // too short for value tick labels
    const tiny = rowH < 15;           // too short even for a channel name

    const window = s.windowSeconds;
    const t1 = this.displayTime;
    const t0 = t1 - window;
    const unit = s.units === 'uv' ? this.meta.uvPerCount : 1;

    const fftMax = showFft ? this.fftCeiling(enabled) : 1;

    if (showRaw && isFinite(t1)) this.drawTimeGrid(ctx, plotX, plotW, areaY, areaH, window);
    if (showFft) this.drawFreqAxis(ctx, plotX, plotW, axTop, s.fftMaxHz);

    for (let k = 0; k < enabled.length; k++) {
      const ch = enabled[k];
      const y0 = areaY + k * rowH;
      this.drawRow(ctx, {
        ch, y0, rowH, plotX, plotW, gutL, gutR, w,
        t0, t1, window, unit, showRaw, showFft, fftMax, dense, tiny,
      });
      if (k) hline(ctx, 0, w, y0, palette.lineSoft);
    }

    // Frame the plot area so the traces read as living inside an instrument
    // rather than floating on the panel.
    vline(ctx, plotX, areaY, areaY + areaH, palette.line);
    vline(ctx, plotX + plotW, areaY, areaY + areaH, palette.line);
    hline(ctx, plotX, plotX + plotW, areaY, palette.line);
    hline(ctx, plotX, plotX + plotW, areaY + areaH, palette.line);

    if (showRaw && isFinite(t1)) {
      this.drawTimeAxis(ctx, plotX, plotW, areaY + areaH, axBot, window);
    }
  }

  /** Peak band power across the channels that share a range. */
  fftCeiling(enabled) {
    let peak = 0;
    for (const ch of enabled) {
      const bands = this.fft[ch];
      if (!bands) continue;
      for (const v of bands) if (v > peak) peak = v;
    }
    return this.fShared.push(Math.max(peak, 1e-9));
  }

  drawRow(ctx, g) {
    const { ch, y0, rowH, plotX, plotW, gutL, w, t0, t1, window, unit } = g;
    const pad = Math.min(3, rowH * 0.1);
    const top = y0 + pad;
    const height = rowH - pad * 2;
    const mid = top + height / 2;
    const s = this.settings;

    // Alternating row wash: enough to separate fourteen rows, not enough to
    // compete with the trace.
    if (ch % 2 === 0) {
      ctx.fillStyle = 'rgba(255,255,255,0.012)';
      ctx.fillRect(0, y0, w, rowH);
    }

    let range = null;
    if (g.showRaw && this.ring.size) {
      range = this.measure(ch, t0, unit);
    }

    ctx.save();
    ctx.beginPath();
    ctx.rect(plotX, y0, plotW, rowH);
    ctx.clip();

    if (g.showFft) this.drawBars(ctx, ch, plotX, plotW, top, height, g.fftMax);

    if (range) {
      hline(ctx, plotX, plotX + plotW, mid, palette.lineSoft);
      this.drawTrace(ctx, ch, plotX, plotW, mid, height, t0, window, range, unit);
    }
    ctx.restore();

    // Left axis: the value scale for the trace.
    if (range && !g.dense) {
      const half = range.half;
      text(ctx, fmt(range.center * (s.removeDc ? 0 : 1) + half, half / 4),
        gutL - 5, top + 6, palette.faint, 'right', 'middle');
      text(ctx, fmt(range.center * (s.removeDc ? 0 : 1) - half, half / 4),
        gutL - 5, top + height - 6, palette.faint, 'right', 'middle');
    } else if (range) {
      text(ctx, `±${fmt(range.half, range.half / 4)}`, gutL - 5, mid,
        palette.faint, 'right', 'middle');
    }

    // Right axis: the band-power scale for the bars.
    if (g.showFft && !g.dense) {
      const ceiling = s.fftShared ? g.fftMax : this.channelCeiling(ch);
      const labelTop = s.fftScale === 'log'
        ? `1e${Math.round(Math.log10(ceiling))}`
        : fmt(ceiling, ceiling / 4);
      text(ctx, labelTop, plotX + plotW + 5, top + 6, palette.bars, 'left', 'middle');
      text(ctx, s.fftScale === 'log' ? `1e${Math.round(Math.log10(ceiling)) - FFT_DECADES}` : '0',
        plotX + plotW + 5, top + height - 6, palette.faint, 'left', 'middle');
    }

    if (!g.tiny) this.drawName(ctx, ch, plotX, top, height, g);
  }

  /** Window mean/extent for one channel, in display units. */
  measure(ch, t0, unit) {
    const ring = this.ring;
    const i0 = ring.lowerBound(t0);
    const n = ring.size - i0;
    if (n < 2) return null;

    const track = ring.data[ch];
    const cap = ring.cap;
    let p = ring.phys(i0);
    let sum = 0;
    let lo = Infinity;
    let hi = -Infinity;
    for (let k = 0; k < n; k++) {
      const v = track[p];
      sum += v;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
      p = p + 1 === cap ? 0 : p + 1;
    }

    const s = this.settings;
    // AC coupling: subtracting the window mean is what makes EEG visible at
    // all -- counts sit on a ~8400 offset, some three orders of magnitude
    // above the rhythms. Same reasoning as the mean removal in dsp.cpp.
    const rawCenter = s.removeDc ? sum / n : (lo + hi) / 2;
    if (!this.vCentered[ch]) {
      this.vCenter[ch] = rawCenter;
      this.vCentered[ch] = 1;
    } else {
      this.vCenter[ch] += (rawCenter - this.vCenter[ch]) * 0.2;
    }
    const center = this.vCenter[ch];

    const peak = Math.max(Math.abs(hi - center), Math.abs(center - lo)) * unit;
    const floor = unit === 1 ? 8 : 4;   // counts vs microvolts
    let half;
    if (s.scale === 'auto') {
      half = this.vRange[ch].push(Math.max(peak * 1.15, floor));
    } else {
      half = Number(s.scale);
    }
    return { i0, n, center, half: Math.max(half, 1e-6) };
  }

  drawTrace(ctx, ch, plotX, plotW, mid, height, t0, window, range, unit) {
    const ring = this.ring;
    const track = ring.data[ch];
    const times = ring.time;
    const cap = ring.cap;
    const { i0, n, center, half } = range;
    const scaleY = (height / 2) / half;
    const scaleX = plotW / window;

    ctx.beginPath();
    ctx.strokeStyle = this.colors[ch];
    ctx.lineWidth = 1.25;
    ctx.lineJoin = 'round';
    ctx.lineCap = 'round';

    if (n > plotW * 2) {
      // More samples than pixels: draw the per-column envelope instead of
      // every point. Plain stride sampling would alias a 10 Hz rhythm into
      // whatever beat frequency it happens to make with the column width;
      // min/max keeps the envelope honest.
      const cols = Math.max(1, Math.ceil(plotW));
      if (this._colMin.length < cols) {
        this._colMin = new Float32Array(cols);
        this._colMax = new Float32Array(cols);
        this._colHit = new Uint8Array(cols);
      }
      const cmin = this._colMin;
      const cmax = this._colMax;
      const hit = this._colHit;
      hit.fill(0, 0, cols);

      let p = ring.phys(i0);
      for (let k = 0; k < n; k++) {
        let col = ((times[p] - t0) * scaleX) | 0;
        if (col < 0) col = 0;
        else if (col >= cols) col = cols - 1;
        const v = track[p];
        if (hit[col]) {
          if (v < cmin[col]) cmin[col] = v;
          else if (v > cmax[col]) cmax[col] = v;
        } else {
          hit[col] = 1;
          cmin[col] = v;
          cmax[col] = v;
        }
        p = p + 1 === cap ? 0 : p + 1;
      }
      let started = false;
      for (let col = 0; col < cols; col++) {
        if (!hit[col]) continue;
        const x = plotX + col;
        const yTop = mid - (cmax[col] - center) * unit * scaleY;
        const yBot = mid - (cmin[col] - center) * unit * scaleY;
        if (!started) { ctx.moveTo(x, yTop); started = true; }
        else ctx.lineTo(x, yTop);
        if (yBot !== yTop) ctx.lineTo(x, yBot);
      }
    } else {
      let p = ring.phys(i0);
      for (let k = 0; k < n; k++) {
        const x = plotX + (times[p] - t0) * scaleX;
        const y = mid - (track[p] - center) * unit * scaleY;
        if (k === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
        p = p + 1 === cap ? 0 : p + 1;
      }
    }
    ctx.stroke();
  }

  channelCeiling(ch) {
    const bands = this.fft[ch];
    let peak = 1e-9;
    if (bands) for (const v of bands) if (v > peak) peak = v;
    return this.fRange[ch].push(peak);
  }

  drawBars(ctx, ch, plotX, plotW, top, height, sharedCeiling) {
    const bands = this.fft[ch];
    if (!bands) return;
    const s = this.settings;
    const ceiling = s.fftShared ? sharedCeiling : this.channelCeiling(ch);
    const maxHz = s.fftMaxHz;
    const bottom = top + height;
    const logHi = Math.log10(Math.max(ceiling, 1e-12));
    const logLo = logHi - FFT_DECADES;

    const heightOf = (power) => {
      if (!(power > 0)) return 0;
      if (s.fftScale === 'log') {
        const l = Math.log10(power);
        if (l <= logLo) return 0;
        return Math.min(1, (l - logLo) / FFT_DECADES) * height;
      }
      return Math.min(1, power / Math.max(ceiling, 1e-12)) * height;
    };

    for (let b = 0; b < this.meta.bands.length; b++) {
      const band = this.meta.bands[b];
      const x0 = plotX + (band.lo / maxHz) * plotW + 1;
      const x1 = plotX + (Math.min(band.hi, maxHz) / maxHz) * plotW - 1;
      if (x1 <= x0) continue;
      const barH = heightOf(bands[b]);
      if (barH < 0.5) continue;
      const y = bottom - barH;

      // Faded on purpose: the bars are the background layer and the trace
      // has to stay readable straight through them.
      const gradient = ctx.createLinearGradient(0, y, 0, bottom);
      gradient.addColorStop(0, 'rgba(240,167,66,0.24)');
      gradient.addColorStop(1, 'rgba(240,167,66,0.05)');
      ctx.fillStyle = gradient;
      ctx.fillRect(x0, y, x1 - x0, barH);
      hline(ctx, x0, x1, y, 'rgba(240,167,66,0.62)');
    }
  }

  drawName(ctx, ch, plotX, top, height, g) {
    const name = this.meta.channels[ch];
    const showQ = this.settings.showQuality && this.hasQuality;
    const value = this.quality[ch];
    const full = this.meta.qualityFullScale;
    const reading = showQ && !g.dense ? String(Math.round(value)) : '';

    ctx.font = FONT_LABEL;
    const nameW = ctx.measureText(name).width;
    ctx.font = FONT;
    const readW = reading ? ctx.measureText(reading).width + 6 : 0;
    const chipH = 14;
    const x = plotX + 3;
    const y = top + 2;
    ctx.fillStyle = 'rgba(7,10,15,0.72)';
    roundRect(ctx, x, y, nameW + readW + 10, chipH, 3);
    ctx.fill();
    text(ctx, name, x + 5, y + chipH / 2 + 0.5, this.colors[ch], 'left', 'middle', FONT_LABEL);
    if (reading) {
      text(ctx, reading, x + 5 + nameW + 6, y + chipH / 2 + 0.5,
        qualityColor(value, full), 'left', 'middle');
    }

    if (showQ) {
      const barX = 5;
      const barW = 4;
      const barTop = top + 2;
      const barH = Math.max(6, height - 4);
      ctx.fillStyle = palette.lineSoft;
      roundRect(ctx, barX, barTop, barW, barH, 2);
      ctx.fill();
      const fillH = Math.max(1, Math.min(1, value / full) * barH);
      ctx.fillStyle = qualityColor(value, full);
      roundRect(ctx, barX, barTop + barH - fillH, barW, fillH, 2);
      ctx.fill();
    }
  }

  drawTimeGrid(ctx, plotX, plotW, areaY, areaH, window) {
    const step = window >= 16 ? 4 : window >= 8 ? 2 : window >= 4 ? 1 : 0.5;
    for (let s = step; s < window; s += step) {
      const x = plotX + plotW * (1 - s / window);
      vline(ctx, x, areaY, areaY + areaH, palette.lineSoft);
    }
  }

  drawTimeAxis(ctx, plotX, plotW, y, axBot, window) {
    const step = window >= 16 ? 4 : window >= 8 ? 2 : window >= 4 ? 1 : 0.5;
    const labelY = y + axBot / 2;
    for (let s = 0; s < window; s += step) {
      const x = plotX + plotW * (1 - s / window);
      vline(ctx, x, y, y + 3, palette.line);
      text(ctx, s === 0 ? 'now' : `-${fmt(s, step)}s`, x, labelY,
        palette.faint, s === 0 ? 'right' : 'center', 'middle');
    }
    const s = this.settings;
    text(ctx, s.units === 'uv' ? 'µV' : 'counts', plotX - 5, labelY,
      palette.dim, 'right', 'middle');
  }

  drawFreqAxis(ctx, plotX, plotW, axTop, maxHz) {
    const y = axTop - 1;
    hline(ctx, plotX, plotX + plotW, y, palette.line);
    for (const hz of niceTicks(0, maxHz, 5)) {
      if (hz > maxHz) continue;
      const x = plotX + (hz / maxHz) * plotW;
      vline(ctx, x, y - 3, y, palette.line);
    }
    // Band names in their own ranges, which is also the legend for the bars.
    for (const band of this.meta.bands) {
      const x0 = plotX + (band.lo / maxHz) * plotW;
      const x1 = plotX + (Math.min(band.hi, maxHz) / maxHz) * plotW;
      if (x1 - x0 < 26) continue;
      text(ctx, band.name, (x0 + x1) / 2, axTop / 2, palette.bars, 'center', 'middle');
    }
    text(ctx, 'Hz', plotX + plotW + 5, axTop / 2, palette.dim, 'left', 'middle');
    text(ctx, 'band', plotX - 5, axTop / 2, palette.dim, 'right', 'middle');
  }
}
