// SPDX-License-Identifier: GPL-2.0-or-later
// A machine's programs in modules of their own (tools/build-title.sh SIDE_PROGRAMS): wasm side modules (position-
// independent, the dylink.0 convention) loaded into the title's memory and function table, as a dynamic linker
// loads a library. The title's module gives them what they import (its functions, its data's addresses, its C stack
// pointer); each is then registered with the machine (rt/microvm-x64/run.c rt_program_register).
const leb = (b, at) => { let v = 0, s = 0, x; do { x = b[at.i++]; v |= (x & 0x7f) << s; s += 7; } while (x & 0x80); return v >>> 0; };
function dylink(module) {
  const sec = WebAssembly.Module.customSections(module, 'dylink.0')[0];
  if (!sec) throw new Error('not a side module (no dylink.0 section)');
  const b = new Uint8Array(sec), at = { i: 0 }, info = { memSize: 0, memAlign: 0, tableSize: 0 };
  while (at.i < b.length) {
    const kind = b[at.i++], len = leb(b, at), end = at.i + len;
    if (kind === 1) { info.memSize = leb(b, at); info.memAlign = leb(b, at); info.tableSize = leb(b, at); }
    at.i = end;
  }
  return info;
}
// bytes: the module's; main: the title's instance; memory: its memory. Returns the side module's instance.
export function loadSide(bytes, main, memory, name) {
  const module = new WebAssembly.Module(bytes), dl = dylink(module), ex = main.exports;
  const memBase = dl.memSize ? ex.rt_side_alloc(dl.memSize, 2 ** dl.memAlign) : 0;
  const table = ex.__indirect_function_table, tableBase = table.length;
  if (dl.tableSize) table.grow(dl.tableSize);
  const env = { memory, __indirect_function_table: table, __stack_pointer: ex.__stack_pointer,
    __memory_base: new WebAssembly.Global({ value: 'i32', mutable: false }, memBase), __table_base: new WebAssembly.Global({ value: 'i32', mutable: false }, tableBase) };
  const got = {}, gotFunc = {};
  for (const i of WebAssembly.Module.imports(module)) {
    if (i.module === 'env' && i.kind === 'function') { if (!ex[i.name]) throw new Error(`${name}: the title exports no ${i.name}`); env[i.name] = ex[i.name]; }
    else if (i.module === 'GOT.mem') { if (!ex[i.name]) throw new Error(`${name}: the title exports no ${i.name}`); got[i.name] = new WebAssembly.Global({ value: 'i32', mutable: true }, ex[i.name].value); }
    else if (i.module === 'GOT.func') { const at = table.length; table.grow(1); table.set(at, ex[i.name]); gotFunc[i.name] = new WebAssembly.Global({ value: 'i32', mutable: true }, at); }
  }
  const side = new WebAssembly.Instance(module, { env, 'GOT.mem': got, 'GOT.func': gotFunc });
  side.exports.__wasm_apply_data_relocs?.(); side.exports.__wasm_call_ctors?.();
  const def = side.exports[`${name}_program`];
  if (!def) throw new Error(`${name}: no ${name}_program`);
  const at = memBase + def.value;   // (a side module exports its data's addresses from its memory base)
  ex.rt_program_register(at);
  if (typeof process !== "undefined" && process.env?.SW_TRACE) console.error(`pisco: ${name}: a module of its own at 0x${memBase.toString(16)}, table from ${tableBase}, its program at 0x${at.toString(16)}`);
  return side;
}
