// SPDX-License-Identifier: GPL-2.0-or-later
// A machine's network in Node (rt/microvm-x64/net.c, SW_NET=1), on the main thread: the frames the guest sends go into
// unkernel's user-mode network stack (third_party/unkernel/net.mjs, MIT), which answers ARP, DHCP, DNS and pings
// itself and makes the guest's TCP connections with this machine's sockets (TCP to the gateway, 10.0.2.2, reaches
// this machine's 127.0.0.1); the frames it sends the guest go into the machine's ring, and its bell is rung.
import { NetStack } from './third_party/unkernel/net.mjs';
import { directBackend } from './third_party/unkernel/node-net.mjs';

// ring: the NetRing's address in the module's memory ({u32 w, r, cap, bell}, then cap bytes of records: a u32 length
// and the frame, to a multiple of 4). Returns { send(frame), ring() }: ring() wakes an idle machine (keys typed too)
export function startNet(memory, ring, { log } = {}) {
  const u32 = new Uint32Array(memory.buffer, ring, 4), i32 = new Int32Array(memory.buffer, ring, 4);
  const cap = u32[2], data = new Uint8Array(memory.buffer, ring + 16, cap);
  let dropped = 0;
  const bell = () => { Atomics.add(i32, 3, 1); Atomics.notify(i32, 3); };
  const put = (frame) => {
    const w = u32[0], need = 4 + ((frame.length + 3) & ~3);
    if (cap - ((w - Atomics.load(u32, 1)) >>> 0) < need) { if (dropped++ === 0 && log) log('net: the ring is full, a frame dropped'); return; }   // (TCP sends it again)
    new DataView(memory.buffer, ring + 16).setUint32(w % cap, frame.length, true);
    for (let k = 0, at = (w + 4) % cap; k < frame.length; k++, at = (at + 1) % cap) data[at] = frame[k];
    Atomics.store(u32, 0, (w + need) >>> 0);
    bell();
  };
  const stack = new NetStack({ backend: directBackend(), hostLoopback: true, log });
  stack.attach({ receiveFrame: put });
  return { send: (frame) => stack.send(frame), ring: bell };
}
