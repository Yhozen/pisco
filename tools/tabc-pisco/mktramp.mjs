// SPDX-License-Identifier: GPL-2.0-or-later
// mktramp.mjs <out.wasm>: the four-slot trampoline the patched kernel's `pisco` imports go to (run.mjs's, as a file)
import binaryen from 'binaryen'; import fs from 'node:fs';
fs.writeFileSync(process.argv[2], binaryen.parseText(`(module
 (type $m (func (param i64) (result i32))) (type $r (func (param i64 i64) (result i32)))
 (type $a (func (param i64 i64 i32) (result i32))) (type $f (func (param i32 i64 i64)))
 (import "js" "tab" (table $t 4 funcref))
 (func $m0 (type $m) (i32.const 0)) (func $r0 (type $r) (unreachable)) (func $a0 (type $a) (local.get 2)) (func $f0 (type $f))
 (elem (i32.const 0) $m0 $r0 $a0 $f0)
 (func (export "mine") (type $m) (call_indirect (type $m) (local.get 0) (i32.const 0)))
 (func (export "run") (type $r) (call_indirect (type $r) (local.get 0) (local.get 1) (i32.const 1)))
 (func (export "after") (type $a) (call_indirect (type $a) (local.get 0) (local.get 1) (local.get 2) (i32.const 2)))
 (func (export "flush") (type $f) (call_indirect (type $f) (local.get 0) (local.get 1) (local.get 2) (i32.const 3))))`).emitBinary());
