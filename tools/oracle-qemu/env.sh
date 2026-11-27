# SPDX-License-Identifier: GPL-2.0-or-later
# env.sh: source it. The machine the oracle boots a kernel on: QEMU's, with its CPU emulated in software (no KVM is
# asked for, so a run is the same on every host).
#   QEMU                 the emulator (default qemu-system-x86_64, a system package: setup.sh says how to get it)
#   ORACLE_QEMU_MACHINE  QEMU's machine and its options (default q35 without its HPET)
#   ORACLE_QEMU_CPU      the CPU model (default Westmere without AES: an Intel CPU with what PISCO's virtual CPU
#                        reports, bases/microvm-x64: SSE up to 4.2, POPCNT, CMPXCHG16B, PCLMULQDQ, no AVX)
#   ORACLE_QEMU_MEMORY   the machine's memory in MiB (default 256)
export QEMU=${QEMU:-qemu-system-x86_64}
export ORACLE_QEMU_MACHINE=${ORACLE_QEMU_MACHINE:-q35,hpet=off}
export ORACLE_QEMU_CPU=${ORACLE_QEMU_CPU:-Westmere,-aes}
export ORACLE_QEMU_MEMORY=${ORACLE_QEMU_MEMORY:-256}
