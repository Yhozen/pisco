// SPDX-License-Identifier: GPL-2.0-or-later
// Plays the app's mixer ring (rt/gdk/xaudio2.c: header words write position, read position, capacity, rate,
// channels; then stereo float frames) from shared memory, keeping a little behind the writer.
class Ring extends AudioWorkletProcessor {
  constructor() { super(); this.port.onmessage = (e) => { const { buffer, addr } = e.data; this.u32 = new Uint32Array(buffer, addr, 16); this.cap = this.u32[2]; this.data = new Float32Array(buffer, addr + 64, this.cap * 2); this.read = -1; }; }
  process(inputs, outputs) {
    const out = outputs[0], n = out[0].length;
    if (!this.u32) return true;
    const w = Atomics.load(this.u32, 0);
    if (this.read < 0 || w - this.read > this.cap / 2 || this.read > w) this.read = Math.max(0, w - 2400);
    for (let i = 0; i < n; i++) {
      if (this.read >= w) { out[0][i] = 0; if (out[1]) out[1][i] = 0; continue; }
      const at = this.read % this.cap; out[0][i] = this.data[2 * at]; if (out[1]) out[1][i] = this.data[2 * at + 1]; this.read++;
    }
    return true;
  }
}
registerProcessor('pisco-ring', Ring);
