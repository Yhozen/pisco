#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build-cowsay.sh [out dir, default out/cowsay]: cowsay (docs/cowsay.md), a Perl script, and what it needs, in
# <out>/root, the root of the app (recipes/cowsay):
#   usr/bin/perl     perl 5.44.0 (GPL-1.0-or-later or Artistic-1.0), static, with its XS modules built in but the
#                    large ones cowsay does not use (Encode, Unicode::Collate, compression, threads, ...); its
#                    library in usr/lib/perl5, without the documentation
#   usr/bin/cowsay   cowsay 3.8.4 (GPL-3.0 or Artistic) and cowthink, its cows in usr/share/cowsay
#   bin/busybox      busybox, for usr/bin/env, which cowsay's #! names (tools/build-neofetch.sh builds it)
# Each is fetched and checked by sha256, built with musl (tools/setup-toolchain.sh musl), not position-independent,
# with its symbols. Nothing of them is in the repository. Environment: MUSL, TOOLS (default ~/tools).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); out=$(mkdir -p "${1:-$root/out/cowsay}" && cd "${1:-$root/out/cowsay}" && pwd)
T=${TOOLS:-$HOME/tools}; MUSL=${MUSL:-$T/musl-1.2.5}
[ -x "$MUSL/bin/musl-gcc" ] || { echo "build-cowsay.sh: no musl in $MUSL (tools/setup-toolchain.sh musl)" >&2; exit 2; }
BB=$root/out/neofetch/build/busybox-1.36.1/busybox_unstripped
[ -x "$BB" ] || "$root/tools/build-neofetch.sh" > /dev/null
fetch() { # url, sha256, file
  mkdir -p "$out/src"; [ -f "$out/src/$3" ] && return
  curl -fsSL -o "$out/src/$3.part" "$1"; echo "$2  $out/src/$3.part" | sha256sum -c --quiet; mv "$out/src/$3.part" "$out/src/$3"
}
fetch https://www.cpan.org/src/5.0/perl-5.44.0.tar.gz 3b855066b92491cb40e86affb1ca57d1a388aa43e51b91c7806a32c2f65f96c3 perl-5.44.0.tar.gz
fetch https://github.com/cowsay-org/cowsay/archive/refs/tags/v3.8.4.tar.gz c15bc10712835d3a9bcda780dc9453362567bf48d1185905dc7ef2334d79aadd cowsay-3.8.4.tar.gz
export SOURCE_DATE_EPOCH=0
P=$out/build/perl-5.44.0
if [ ! -x "$P/perl" ]; then
  rm -rf "$P"; mkdir -p "$out/build"; tar -xzf "$out/src/perl-5.44.0.tar.gz" -C "$out/build"
  # (Time::HiRes's probes do not link statically; the rest left out is large and unused)
  NOEXT="Time/HiRes Encode Encode/Byte Encode/CN Encode/EBCDIC Encode/JP Encode/KR Encode/Symbol Encode/TW Encode/Unicode Unicode/Collate Unicode/Normalize
    Compress/Raw/Bzip2 Compress/Raw/Zlib Digest/MD5 Digest/SHA Devel/PPPort Devel/Peek Math/BigInt/FastCalc Storable threads threads/shared SDBM_File Sys/Syslog
    Time/Piece Socket Opcode PerlIO/encoding I18N/Langinfo B"
  (cd "$P" && ./Configure -des -Dcc="$MUSL/bin/musl-gcc" -Dprefix=/usr -Uusedl -Uuseithreads -Dldflags="-static -no-pie" -Accflags="-fno-stack-protector -fno-pie" \
     -Doptimize=-O2 -Dman1dir=none -Dman3dir=none -Dlocincpth= -Dloclibpth= -Dlibpth="$MUSL/lib" -Dplibpth="$MUSL/lib" -Dglibpth= -Dlibc="$MUSL/lib/libc.a" -Dnoextensions="$(echo $NOEXT)" > configure.log 2>&1 \
   && make -j "$(nproc)" > make.log 2>&1) || { echo "build-cowsay.sh: perl did not build (see $P)" >&2; exit 1; }
fi
R=$out/root; rm -rf "$R"; mkdir -p "$R/bin" "$R/usr/bin"
make -C "$P" install DESTDIR="$R" > "$out/build/perl-install.log" 2>&1
# what cowsay runs: perl and its library; not the tools perl installs, nor the documentation
find "$R/usr/bin" -type f ! -name perl -delete; rm -f "$R/usr/bin/perl5.44.0"
find "$R/usr/lib/perl5" \( -name '*.pod' -o -name '*.h' -o -name '*.a' \) -delete; rm -rf "$R/usr/lib/perl5/5.44.0/pod" "$R/usr/lib/perl5/5.44.0/x86_64-linux/CORE"
[ -d "$out/build/cowsay-3.8.4" ] || tar -xzf "$out/src/cowsay-3.8.4.tar.gz" -C "$out/build"
make -C "$out/build/cowsay-3.8.4" install prefix=/usr DESTDIR="$R" > "$out/build/cowsay-install.log" 2>&1
rm -f "$R/usr/bin/cowthink"; cp "$R/usr/bin/cowsay" "$R/usr/bin/cowthink"; rm -rf "$R/usr/share/man" "$R/usr/share/doc"
cp "$BB" "$R/bin/busybox"; printf '#!/bin/busybox\n' > "$R/usr/bin/env"; chmod 755 "$R/usr/bin/env" "$R/usr/bin/cowthink"
echo "$R"
