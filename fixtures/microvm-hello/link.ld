/* SPDX-License-Identifier: GPL-2.0-or-later */
/* fixtures/microvm-hello: linked as Linux is, its kernel at 0xffffffff81000000 and loaded at 16 MiB */
ENTRY(common_startup_64)
PHDRS { boot PT_LOAD; user PT_LOAD; kernel PT_LOAD; note PT_NOTE; }
SECTIONS {
  . = 0x100000;
  .boot32 : { *(.boot32) } :boot
  . = 0x400000;
  .user : { *(.user) } :user
  .user.data : { *(.user.data) } :user
  . = 0xffffffff81000000;
  .text : AT(0x1000000) { *(.text .text.*) } :kernel
  .rodata : { *(.rodata .rodata.*) } :kernel
  .note.Xen : { *(.note.Xen) } :kernel :note
  .data : { *(.data .data.*) } :kernel
  .bss : { *(.bss .bss.*) *(COMMON) } :kernel
  _end = .;
  /DISCARD/ : { *(.eh_frame) *(.comment) *(.note.GNU-stack) *(.note.gnu.*) }
}
