// D3D12.X memory operations on guest memory (rt/d3d12_cmd.h GPU_L_MEM_*), shared by the WebGPU and null back
// ends. Guest addresses may carry a tag above 4 GiB (the console's GPU-visible region): the low 32 bits name
// the byte. They run once the work before them is done (the back end's business).
export const lo32 = (lo) => lo >>> 0;
const cmp = (f, a, b) => [false, a < b, a === b, a <= b, a > b, a !== b, a >= b, true][f - 1] ?? false;
export function memWrite(g, a) {
  g.refresh(); const at = lo32(a[0]);
  if (a[4] === 8) {
    const v = a[5] ? BigInt(Math.round((performance.timeOrigin + performance.now()) * 1000)) : BigInt(a[2]) | BigInt(a[3]) << 32n;
    g.dv.setBigUint64(at, v, true);
  } else g.dv.setUint32(at, a[2], true);
}
export function memFill(g, a) {
  g.refresh(); const at = lo32(a[0]), n = a[2] + a[3] * 4294967296;
  if (at % 4 === 0 && n % 4 === 0) g.u32.fill(a[4], at >>> 2, (at + n) >>> 2);
  else for (let i = 0; i < n; i++) g.u8[at + i] = (a[4] >>> (8 * (i & 3))) & 0xFF;
}
export function memCopy(g, a) { g.refresh(); const d = lo32(a[0]), s = lo32(a[2]), n = a[4] + a[5] * 4294967296; g.u8.copyWithin(d, s, s + n); }
// true when the comparison holds: (memory & mask) <func> reference
export function memTest(g, a) {
  g.refresh(); const at = lo32(a[0]);
  if (a[7] === 8) { const m = BigInt(a[5]) | BigInt(a[6]) << 32n, r = BigInt(a[3]) | BigInt(a[4]) << 32n; return cmp(a[2], g.dv.getBigUint64(at, true) & m, r & m); }
  const m = a[5] >>> 0; return cmp(a[2], (g.dv.getUint32(at, true) & m) >>> 0, (a[3] & m) >>> 0);
}
export async function memWait(g, a, log) {
  const t0 = Date.now(); let told = false;
  while (!memTest(g, a)) {
    await new Promise((r) => setTimeout(r, 1));
    if (!told && Date.now() - t0 > 5000) { told = true; log?.(`gpu: Wait${a[7] * 8}BitValueX at 0x${lo32(a[0]).toString(16)} still waiting after 5 s`); }
  }
}
