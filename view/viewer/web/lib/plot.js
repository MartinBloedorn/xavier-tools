/* Canvas plotting primitives shared by the widgets.
 *
 * No charting library. Fourteen stacked realtime traces with a second,
 * independently scaled bar chart overlaid on each is not a shape general
 * charting libraries do well, and at 60 fps the ones that re-layout per
 * frame do not keep up at all.
 */

/** Read the palette out of the stylesheet, so CSS stays the single source. */
const CSS = getComputedStyle(document.documentElement);
const token = (name, fallback) => (CSS.getPropertyValue(name).trim() || fallback);

export const palette = {
  ink:      token('--ink', '#d5dce6'),
  dim:      token('--ink-dim', '#8b95a5'),
  faint:    token('--ink-faint', '#5a6373'),
  line:     token('--line', '#232b39'),
  lineSoft: token('--line-soft', '#1a202b'),
  panel:    token('--panel', '#11151d'),
  sunken:   token('--sunken', '#070a0f'),
  trace:    token('--trace', '#2dd4bf'),
  bars:     token('--bars', '#f0a742'),
  angle:    token('--angle', '#a78bfa'),
  rate:     token('--rate', '#38bdf8'),
  pad:      token('--pad', '#f472b6'),
  focus:    token('--focus', '#38bdf8'),
  relax:    token('--relax', '#a78bfa'),
  ok:       token('--ok', '#3fb950'),
  warn:     token('--warn', '#d6a02a'),
  bad:      token('--bad', '#f2544b'),
};

export const FONT = '10px ui-monospace, SFMono-Regular, Menlo, Consolas, monospace';
export const FONT_LABEL = '11px ui-sans-serif, system-ui, -apple-system, "Segoe UI", sans-serif';

/** A gentle cyan-to-green ramp across the electrodes.
 *
 * Narrow on purpose: the point is to help the eye follow a row across the
 * stack, not to identify a channel by colour -- the labels do that, and
 * fourteen maximally distinct hues would look like a bag of sweets.
 */
export function channelColor(i, n) {
  const t = n > 1 ? i / (n - 1) : 0;
  return `hsl(${188 - 56 * t} 68% 58%)`;
}

/** Size the backing store to the element's real pixels. Returns null when
 * the element has no area yet, which happens on the first frame after a
 * widget opens. */
export function fitCanvas(canvas) {
  const rect = canvas.getBoundingClientRect();
  if (rect.width < 2 || rect.height < 2) return null;
  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const w = Math.round(rect.width * dpr);
  const h = Math.round(rect.height * dpr);
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, w: rect.width, h: rect.height, dpr };
}

/** Tick positions on a "nice" 1/2/5 x 10^k step covering [min, max]. */
export function niceTicks(min, max, target = 5) {
  if (!isFinite(min) || !isFinite(max) || max <= min) return [];
  const raw = (max - min) / Math.max(1, target);
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const norm = raw / mag;
  const step = (norm >= 5 ? 10 : norm >= 2 ? 5 : norm >= 1 ? 2 : 1) * mag;
  const out = [];
  for (let v = Math.ceil(min / step) * step; v <= max + step * 1e-6; v += step) {
    out.push(Math.abs(v) < step * 1e-9 ? 0 : v);
  }
  return out;
}

/** Compact axis label: 3 significant figures, switching to exponent form
 * only when a fixed rendering would be misleadingly long. */
export function fmt(value, step) {
  if (!isFinite(value)) return '--';
  const a = Math.abs(value);
  if (a === 0) return '0';
  if (a >= 1e5 || a < 1e-3) return value.toExponential(0).replace('e+', 'e');
  const decimals = step !== undefined
    ? Math.max(0, Math.min(3, -Math.floor(Math.log10(Math.abs(step)))))
    : (a >= 100 ? 0 : a >= 10 ? 1 : 2);
  return value.toFixed(decimals);
}

/** Crisp 1px line: canvas strokes straddle the coordinate, so a whole
 * number puts half a pixel either side and renders as a 2px blur. */
export const crisp = (v) => Math.round(v) + 0.5;

export function hline(ctx, x0, x1, y, color, width = 1) {
  ctx.beginPath();
  ctx.strokeStyle = color;
  ctx.lineWidth = width;
  ctx.moveTo(x0, crisp(y));
  ctx.lineTo(x1, crisp(y));
  ctx.stroke();
}

export function vline(ctx, x, y0, y1, color, width = 1) {
  ctx.beginPath();
  ctx.strokeStyle = color;
  ctx.lineWidth = width;
  ctx.moveTo(crisp(x), y0);
  ctx.lineTo(crisp(x), y1);
  ctx.stroke();
}

export function text(ctx, str, x, y, color, align = 'left', baseline = 'middle', font = FONT) {
  ctx.font = font;
  ctx.fillStyle = color;
  ctx.textAlign = align;
  ctx.textBaseline = baseline;
  ctx.fillText(str, x, y);
}

/** Centre a short message in a region -- "waiting for data", and friends. */
export function placeholder(ctx, str, x, y, w, h) {
  text(ctx, str, x + w / 2, y + h / 2, palette.faint, 'center', 'middle', FONT_LABEL);
}

export function roundRect(ctx, x, y, w, h, r) {
  const rr = Math.min(r, w / 2, h / 2);
  ctx.beginPath();
  ctx.moveTo(x + rr, y);
  ctx.arcTo(x + w, y, x + w, y + h, rr);
  ctx.arcTo(x + w, y + h, x, y + h, rr);
  ctx.arcTo(x, y + h, x, y, rr);
  ctx.arcTo(x, y, x + w, y, rr);
  ctx.closePath();
}

/** Contact-quality colour, red through amber to green.
 *
 * Full scale is emokit's "> 4000 is a good contact", which is a display
 * convention and not a calibrated threshold -- see epoc/doc/protocol.md.
 */
export function qualityColor(value, fullScale) {
  const q = Math.max(0, Math.min(1, value / fullScale));
  if (q < 0.35) return palette.bad;
  if (q < 0.7) return palette.warn;
  return palette.ok;
}

/** An exponential smoother for display ranges.
 *
 * Auto-scaling straight off each frame's min/max makes a trace jitter
 * vertically as the extremes wander; easing the limit removes that without
 * the lag of a long window. Growth is faster than decay so a sudden
 * artefact comes into view immediately rather than clipping for a second.
 */
export class Smoothed {
  constructor(value = 0, riseRate = 0.35, fallRate = 0.04) {
    this.value = value;
    this.rise = riseRate;
    this.fall = fallRate;
    this.primed = false;
  }
  push(target) {
    if (!isFinite(target)) return this.value;
    if (!this.primed) {
      this.primed = true;
      this.value = target;
      return this.value;
    }
    const rate = target > this.value ? this.rise : this.fall;
    this.value += (target - this.value) * rate;
    return this.value;
  }
  reset(value = 0) {
    this.value = value;
    this.primed = false;
  }
}
