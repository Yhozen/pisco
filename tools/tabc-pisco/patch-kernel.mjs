// SPDX-License-Identifier: GPL-2.0-or-later
// patch-kernel.mjs <unkernel.wasm> <out.wasm>: unkernel's kernel with a second CPU beside kwemu, without rebuilding it.
// kw_run (the kernel's only entry into user code, arch/wasm/kernel/traps.c user_mode_loop) asks pisco.mine(regs)
// first: a task whose kw_regs say "x86-64" runs on pisco.run (lifted code); any other runs on kwemu as before
// (kw_run_a64, the original body), and pisco.after sees what kwemu returned (the loader's request to enter x86-64).
// kwemu's TLB flushes (kw_flush_tlb_range, kw_flush_tlb_all, kw_set_pgd) also call pisco.flush, so the lifted
// code's TLB follows the kernel's page tables.
import binaryen from 'binaryen';
import fs from 'node:fs';
const [, , inPath, outPath] = process.argv;
const orig = fs.readFileSync(inPath);
const mod = binaryen.readBinary(orig);
mod.setFeatures(mod.getFeatures());
const { i32, i64, none } = binaryen;
const T = (...t) => binaryen.createType(t);
mod.addFunctionImport('pisco_mine', 'pisco', 'mine', T(i64), i32);
mod.addFunctionImport('pisco_run', 'pisco', 'run', T(i64, i64), i32);
mod.addFunctionImport('pisco_after', 'pisco', 'after', T(i64, i64, i32), i32);
mod.addFunctionImport('pisco_flush', 'pisco', 'flush', T(i32, i64, i64), none);

function rewrite(name, makeBody, keepAs) {
  const f = mod.getFunction(name), info = binaryen.getFunctionInfo(f);
  const old = info.body;
  if (keepAs) mod.addFunction(keepAs, info.params, info.results, info.vars, mod.copyExpression(old));
  const body = makeBody(old);
  binaryen.Function.setBody(f, body);
}
const L = (i, t) => mod.local.get(i, t);
rewrite('kw_run', () => mod.block(null, [
  mod.if(mod.call('pisco_mine', [L(1, i64)], i32), mod.return(mod.call('pisco_run', [L(0, i64), L(1, i64)], i32))),
  mod.return(mod.call('pisco_after', [L(0, i64), L(1, i64), mod.call('kw_run_a64', [L(0, i64), L(1, i64)], i32)], i32)),
], i32), 'kw_run_a64');
rewrite('kw_flush_tlb_range', (old) => mod.block(null, [mod.call('pisco_flush', [mod.i32.const(1), L(1, i64), L(2, i64)], none), old], none));
rewrite('kw_flush_tlb_all', (old) => mod.block(null, [mod.call('pisco_flush', [mod.i32.const(2), mod.i64.const(0n), mod.i64.const(0n)], none), old], none));
rewrite('kw_set_pgd', (old) => mod.block(null, [mod.call('pisco_flush', [mod.i32.const(3), L(1, i64), mod.i64.const(0n)], none), old], none));

binaryen.setDebugInfo(true);   // keep the name section
const out = mod.emitBinary();
fs.writeFileSync(outPath, out);
// what the SDK reads from custom sections must be unchanged
const a = new WebAssembly.Module(orig), b = new WebAssembly.Module(out);
for (const s of ['unkernel.initrd', 'unkernel.cmdline', 'unkernel.datadisk', 'unkernel.kernel.smp.kwpatch', 'target_features']) {
  const x = WebAssembly.Module.customSections(a, s), y = WebAssembly.Module.customSections(b, s);
  const same = x.length === y.length && x.every((v, i) => Buffer.from(v).equals(Buffer.from(y[i])));
  console.log(`section ${s}: ${x.length} -> ${y.length}${same ? '' : ' DIFFERENT'}`);
}
console.log(`${inPath} (${orig.length}) -> ${outPath} (${out.length})`);
