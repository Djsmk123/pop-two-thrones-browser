// Plays the game's mixed audio: a ring of stereo float frames in the module's shared memory
// (see rc_audio_ring in host/dsound.c: struct { u32 w, r, rate, frames; f32 data[frames * 2] }).
// The game thread keeps the ring topped up to about 80 ms. If it ever runs dry, the output fades to silence
// instead of cutting off (a hard cut is a click), waits until a little has queued, then fades back in.
const START_FRAMES = 1024;   // queued frames needed before playing (re)starts
class RcRing extends AudioWorkletProcessor {
  constructor() {
    super();
    this.memory = null;
    this.base = 0;
    this.playing = false;
    this.gain = 0;           // current fade multiplier, 0..1
    this.last = [0, 0];
    this.port.onmessage = (e) => { this.memory = e.data.memory; this.base = e.data.ring; };
  }
  process(inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out[1] || out[0];
    if (!this.memory) { L.fill(0); R.fill(0); return true; }
    const buf = this.memory.buffer;   // a new buffer object after memory growth
    if (!this.u32 || this.u32.buffer !== buf) { this.u32 = new Uint32Array(buf); this.f32 = new Float32Array(buf); }
    const hdr = this.base >> 2, frames = this.u32[hdr + 3], data = hdr + 4;
    const w = Atomics.load(this.u32, hdr), r = Atomics.load(this.u32, hdr + 1);
    const avail = (w - r) >>> 0, len = L.length;
    if (!this.playing && avail >= START_FRAMES) this.playing = true;
    if (this.playing && avail < len) this.playing = false;   // would underrun inside this block
    const step = 1 / 128;
    let used = 0;
    for (let i = 0; i < len; i++) {
      if (this.playing) this.gain = Math.min(1, this.gain + step); else this.gain = Math.max(0, this.gain - step);
      let l = 0, rr = 0;
      if (this.playing) {
        const k = ((r + used) >>> 0) % frames; used++;
        l = this.f32[data + 2 * k]; rr = this.f32[data + 2 * k + 1];
        this.last[0] = l; this.last[1] = rr;
      } else if (this.gain > 0) { l = this.last[0]; rr = this.last[1]; }   // hold the last sample while fading out
      L[i] = l * this.gain; R[i] = rr * this.gain;
    }
    Atomics.store(this.u32, hdr + 1, (r + used) >>> 0);
    return true;
  }
}
registerProcessor('rc-ring', RcRing);
