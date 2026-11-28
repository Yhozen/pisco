#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# tabc-x86-prelude.sh, sourced in tabc's arm64 container (tools/tabc-poc/bench.mjs tabc-x86, from TABC_VOL as /opt/x/prelude.sh): an
# amd64 root at /x86, Alpine's amd64 layer, every x86-64 ELF in it run by qemu-user through binfmt_misc (F: the
# interpreter opened now, so it works inside the chroot). TABC_VOL holds x86root/, qemu-x86_64, bench-aarch64, workload.
cp -a /opt/x/x86root /x86; cp /opt/x/qemu-x86_64 /usr/bin/qemu-x86_64; cp /lib/ld-musl-aarch64.so.1 /x86/lib/
mount -t binfmt_misc none /proc/sys/fs/binfmt_misc 2>/dev/null
printf '%s' ':qemu-x86_64:M::\x7fELF\x02\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x3e\x00:\xff\xff\xff\xff\xff\xfe\xfe\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/usr/bin/qemu-x86_64:F' > /proc/sys/fs/binfmt_misc/register
mount -t proc proc /x86/proc; mount -t tmpfs tmp /x86/tmp; mount -o bind /dev /x86/dev
