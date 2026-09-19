/* Fixed-capacity ring buffers for time series.
 *
 * One shared Float64Array of timestamps and N parallel Float32Arrays of
 * values, because every channel in a /raw/all message shares one timestamp
 * -- storing it fourteen times would triple the memory for nothing.
 *
 * Typed arrays rather than arrays of objects: at 128 Hz over 60 s that is
 * 7680 samples per channel, and the draw loop walks most of them every
 * frame. Object allocation at that rate is what makes canvas plotting
 * stutter.
 */

export class SampleRing {
  /**
   * @param {number} series how many parallel value tracks
   * @param {number} capacity samples retained before the oldest is dropped
   */
  constructor(series, capacity) {
    this.cap = capacity;
    this.time = new Float64Array(capacity);
    this.data = [];
    for (let s = 0; s < series; s++) this.data.push(new Float32Array(capacity));
    this.written = 0;   // total ever pushed; may exceed capacity
    this.head = 0;      // next physical write position
  }

  /** Number of samples currently retained. */
  get size() {
    return this.written < this.cap ? this.written : this.cap;
  }

  /** Physical index of logical sample `i`, where 0 is the oldest retained.
   *
   * Hot loops should call this once for their start index and then step the
   * physical index forward themselves, wrapping at `cap` -- a modulo per
   * sample per channel is a measurable cost at these sizes.
   */
  phys(i) {
    const start = this.head - this.size;
    let p = (start + i) % this.cap;
    if (p < 0) p += this.cap;
    return p;
  }

  push(t, values) {
    const p = this.head;
    this.time[p] = t;
    for (let s = 0; s < this.data.length; s++) this.data[s][p] = values[s];
    this.head = p + 1 === this.cap ? 0 : p + 1;
    this.written++;
  }

  /** Overwrite one track at a logical index. Used when a derived series --
   * the integrated gyro angle -- is recomputed over existing history. */
  setAt(series, i, value) {
    this.data[series][this.phys(i)] = value;
  }

  timeAt(i) { return this.time[this.phys(i)]; }
  valueAt(series, i) { return this.data[series][this.phys(i)]; }

  get latestTime() {
    return this.size ? this.time[this.head === 0 ? this.cap - 1 : this.head - 1] : NaN;
  }

  clear() {
    this.written = 0;
    this.head = 0;
  }

  /** First logical index whose timestamp is >= `target`, or `size` if none.
   * Timestamps are monotonic, so a binary search is valid. */
  lowerBound(target) {
    let lo = 0;
    let hi = this.size;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (this.timeAt(mid) < target) lo = mid + 1;
      else hi = mid;
    }
    return lo;
  }
}
