#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build-linux.sh [out dir, default out/linux]: the Linux title's files (docs/linux.md). Nothing of them is in the
# repository:
#   vmlinux         Linux 6.18.65 (GPL-2.0-only), built from kernel.org's source with recipes/linux/kernel.config on
#                   top of the kernel's own tinyconfig, without jump tables (KCFLAGS): what the lifter reads
#   bzImage         the same kernel as a boot loader takes it: what QEMU boots (tools/oracle-qemu)
#   config          the configuration it was built with
#   initramfs.cpio  /init (recipes/linux/init), busybox (tools/build-busybox.sh) and a link to it for each applet,
#                   uncompressed, written by the kernel's own gen_init_cpio
# The source is fetched once and checked by sha256. The build names no user, host or time, so two builds on one
# machine give the same files.
# Needs gcc, make, flex, bison, bc and libelf's headers (tools/setup-toolchain.sh kernel says which are missing),
# and musl for busybox. Environment: JOBS (default: every CPU), LINUX_VERSION and LINUX_SHA256 (another release).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); out=$(mkdir -p "${1:-$root/out/linux}" && cd "${1:-$root/out/linux}" && pwd)
V=${LINUX_VERSION:-6.18.65}; SHA=${LINUX_SHA256:-c92606f956f09dc50b5a0e2af9e86763fce27ada1647de97f8d1ac8cfc95801d}
JOBS=${JOBS:-$(nproc)}
for t in gcc make flex bison bc; do command -v $t > /dev/null || { echo "build-linux.sh: $t is missing (tools/setup-toolchain.sh kernel)" >&2; exit 2; }; done
src=$out/build/linux-$V; obj=$out/build/obj-$V
if [ ! -f "$src/Makefile" ]; then
  f=$out/src/linux-$V.tar.xz; mkdir -p "$out/src" "$out/build"
  [ -f "$f" ] || { curl -fsSL -o "$f.part" "https://cdn.kernel.org/pub/linux/kernel/v${V%%.*}.x/linux-$V.tar.xz"; echo "$SHA  $f.part" | sha256sum -c --quiet; mv "$f.part" "$f"; }
  rm -rf "$src"; tar -xJf "$f" -C "$out/build"
fi
export KBUILD_BUILD_TIMESTAMP='Thu Jan  1 00:00:00 UTC 1970' KBUILD_BUILD_USER=pisco KBUILD_BUILD_HOST=pisco KBUILD_BUILD_VERSION=1 SOURCE_DATE_EPOCH=0
mk() { make -C "$src" O="$obj" ARCH=x86_64 "$@"; }
want=$root/recipes/linux/kernel.config
if [ ! -f "$obj/.config" ] || [ "$want" -nt "$obj/.config" ]; then
  mkdir -p "$obj"; mk tinyconfig > "$out/config.log" 2>&1
  (cd "$obj" && "$src/scripts/kconfig/merge_config.sh" -m -O . .config "$want" >> "$out/config.log" 2>&1)
  mk olddefconfig >> "$out/config.log" 2>&1
fi
# every line of the recipe's configuration must be in the kernel's, as written
bad=0
while IFS= read -r line; do
  case $line in
    CONFIG_*=*) grep -qxF "$line" "$obj/.config" || { echo "build-linux.sh: the kernel did not take $line" >&2; bad=1; } ;;
    "# CONFIG_"*" is not set") n=${line#\# }; n=${n% is not set}; ! grep -q "^$n=" "$obj/.config" || { echo "build-linux.sh: the kernel kept $(grep "^$n=" "$obj/.config") ($line)" >&2; bad=1; } ;;
  esac
done < "$want"
[ $bad = 0 ] || exit 1
mk -j "$JOBS" KCFLAGS=-fno-jump-tables vmlinux bzImage > "$out/make.log" 2>&1 || { echo "build-linux.sh: the kernel did not build (see $out/make.log)" >&2; tail -5 "$out/make.log" >&2; exit 1; }
cp "$obj/vmlinux" "$out/vmlinux"; cp "$obj/arch/x86/boot/bzImage" "$out/bzImage"; cp "$obj/.config" "$out/config"

# the initramfs: a list for gen_init_cpio (every file owned by root, at time 0)
bb=$("$root/tools/build-busybox.sh" "$out")
{ echo "dir /bin 755 0 0"; for d in dev proc sys tmp etc root; do echo "dir /$d 755 0 0"; done
  echo "nod /dev/console 600 0 0 c 5 1"
  echo "file /bin/busybox $bb 755 0 0"
  for a in $("$bb" --list); do echo "slink /bin/$a busybox 777 0 0"; done
  echo "file /init $root/recipes/linux/init 755 0 0"; } > "$out/initramfs.list"
"$obj/usr/gen_init_cpio" -t 0 "$out/initramfs.list" > "$out/initramfs.cpio"
echo "$out: vmlinux ($(stat -c %s "$out/vmlinux") bytes), bzImage, initramfs.cpio ($(stat -c %s "$out/initramfs.cpio") bytes)"
