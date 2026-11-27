// SPDX-License-Identifier: GPL-2.0-or-later
// fixtures/microvm-hello: the assembly of a small x86-64 kernel that checks the machine rt/microvm-x64 imitates
// (docs/linux.md, K1). It is linked as Linux is: its code at 0xffffffff81000000, loaded at 16 MiB.
//   QEMU enters it in 32-bit mode at pvh_start (its PVH note): paging and long mode on, then common_startup_64.
//   PISCO enters common_startup_64 directly, with CR3 on early_top_pgt, as it will enter Linux.
// The 32-bit start is in a section that is not code to the lifter; everything else is lifted.
#define KOFF 0xffffffff80000000

	.section .note.Xen, "a"
	.align 4
	.long 4, 4, 18            // name size, descriptor size, XEN_ELFNOTE_PHYS32_ENTRY
	.asciz "Xen"
	.long pvh_start

// ---- the 32-bit start (QEMU only), at physical 1 MiB ----
	.section .boot32, "a"
	.code32
	.globl pvh_start
pvh_start:
	cli
	movl $(early_top_pgt - KOFF), %eax
	movl %eax, %cr3
	movl %cr4, %eax
	orl $0x20, %eax           // PAE
	movl %eax, %cr4
	movl $0xc0000080, %ecx    // EFER: long mode, system calls
	rdmsr
	orl $0x101, %eax
	wrmsr
	movl %cr0, %eax
	orl $0x80010001, %eax     // paging, write protection, protected mode
	movl %eax, %cr0
	lgdt boot_gdt_ptr
	ljmp $0x08, $pvh_64
	.code64
pvh_64:
	movabsq $common_startup_64, %rax
	jmp *%rax
	.align 8
boot_gdt:
	.quad 0, 0x00af9a000000ffff, 0x00cf92000000ffff
boot_gdt_ptr:
	.word 23
	.long boot_gdt

// ---- the kernel ----
	.text
	.globl common_startup_64
common_startup_64:
	leaq boot_stack_top(%rip), %rsp
	lgdt gdt_ptr(%rip)
	movl $0x10, %eax
	movl %eax, %ds
	movl %eax, %es
	movl %eax, %ss
	xorl %eax, %eax
	movl %eax, %fs
	movl %eax, %gs
	xorl %ebp, %ebp
	call kmain
1:	hlt
	jmp 1b

// interrupt and exception entries: a frame of struct regs (hello.c) on the stack, trap(), back with iretq
.macro ISR n, err
	.globl isr\n
isr\n:
	.if \err == 0
	pushq $0
	.endif
	pushq $\n
	jmp isr_common
.endm
	ISR 3, 0
	ISR 6, 0
	ISR 13, 1
	ISR 14, 1
	ISR 32, 0
.macro SAVE
	pushq %rax; pushq %rbx; pushq %rcx; pushq %rdx; pushq %rsi; pushq %rdi; pushq %rbp
	pushq %r8; pushq %r9; pushq %r10; pushq %r11; pushq %r12; pushq %r13; pushq %r14; pushq %r15
.endm
.macro RESTORE
	popq %r15; popq %r14; popq %r13; popq %r12; popq %r11; popq %r10; popq %r9; popq %r8
	popq %rbp; popq %rdi; popq %rsi; popq %rdx; popq %rcx; popq %rbx; popq %rax
.endm
	.globl isr_common
isr_common:
	SAVE
	movq %rsp, %rdi
	cld
	call trap
	RESTORE
	addq $16, %rsp
	iretq

// the system call entry (LSTAR): a frame like an interrupt's, on the kernel stack of the task, back with sysretq
	.globl syscall_entry
syscall_entry:
	movq %rsp, user_rsp(%rip)
	movq kernel_rsp0(%rip), %rsp
	pushq $0x1b
	pushq user_rsp(%rip)
	pushq %r11
	pushq $0x23
	pushq %rcx
	pushq $0
	pushq $0x80
	SAVE
	movq %rsp, %rdi
	call syscall_c
	RESTORE
	addq $16, %rsp
	popq %rcx
	addq $8, %rsp
	popq %r11
	popq %rsp
	sysretq

// switch_to(prev, next): the callee-saved registers on the stack, the stack pointer in the task (as Linux's)
	.globl __switch_to_asm
__switch_to_asm:
	pushq %rbp; pushq %rbx; pushq %r12; pushq %r13; pushq %r14; pushq %r15
	movq %rsp, (%rdi)
	movq (%rsi), %rsp
	popq %r15; popq %r14; popq %r13; popq %r12; popq %rbx; popq %rbp
	ret

// a new task's first return: to ring 3, at user_main, with interrupts on
	.globl enter_user
enter_user:
	pushq $0x1b
	pushq $0x800000
	pushq $0x202
	pushq $0x23
	pushq $user_main
	iretq

// u64 probe_read(u64 *p): *p, or -14 when reading faults (trap() moves the faulting instruction on to the fix-up)
	.globl probe_read, probe_read_insn, probe_read_fixup
probe_read:
probe_read_insn:
	movq (%rdi), %rax
	ret
probe_read_fixup:
	movq $-14, %rax
	ret

// ---- ring 3: code and data in the user pages (identity-mapped at 4 MiB, user-accessible) ----
	.section .user, "ax"
	.globl user_main, user_preempted
user_main:
	movl $1, %eax
	movl $1, %edi
	leaq umsg1(%rip), %rsi
	movl $umsg1_len, %edx
	syscall
1:	incq %rbx                 // until the timer takes the CPU away
	jmp 1b
user_preempted:
	movl $1, %eax
	movl $1, %edi
	leaq umsg2(%rip), %rsi
	movl $umsg2_len, %edx
	syscall
	movl $60, %eax
	movl $7, %edi
	syscall
2:	jmp 2b
	.section .user.data, "a"
umsg1:	.ascii "user: hello from ring 3\n"
	umsg1_len = . - umsg1
umsg2:	.ascii "user: preempted, exiting\n"
	umsg2_len = . - umsg2

// ---- tables ----
	.data
	.align 16
gdt:
	.quad 0
	.quad 0x00af9a000000ffff  // 0x08 kernel code
	.quad 0x00cf92000000ffff  // 0x10 kernel data
	.quad 0x00cff2000000ffff  // 0x18 user data (0x1b)
	.quad 0x00affa000000ffff  // 0x20 user code (0x23)
	.globl tss_desc
tss_desc:
	.quad 0, 0                // 0x28 the TSS (hello.c fills it)
gdt_end:
gdt_ptr:
	.word gdt_end - gdt - 1
	.quad gdt

// page tables: the low 1 GiB identity-mapped (2 MiB pages; 4 to 8 MiB user-accessible), the same at
// 0xffffffff80000000, and 3 to 4 GiB identity-mapped uncached for the APIC
	.align 4096
	.globl early_top_pgt
early_top_pgt:
	.quad ident_pdpt - KOFF + 7
	.fill 401, 8, 0
	.globl dyn_slot
dyn_slot:                         // [402]: 0xffffc90000000000, for the checks' own mappings
	.quad 0
	.fill 108, 8, 0
	.quad high_pdpt - KOFF + 3
	.align 4096
ident_pdpt:
	.quad ident_pd0 - KOFF + 7
	.quad 0, 0
	.quad ident_pd3 - KOFF + 3
	.fill 508, 8, 0
	.align 4096
high_pdpt:
	.fill 510, 8, 0
	.quad ident_pd0 - KOFF + 3
	.quad 0
	.align 4096
ident_pd0:
	.quad 0x000083, 0x200083, 0x400087, 0x600087
	i = 4
	.rept 508
	.quad (i << 21) + 0x83
	i = i + 1
	.endr
	.align 4096
ident_pd3:
	i = 0
	.rept 512
	.quad 0xc0000000 + (i << 21) + 0x9b
	i = i + 1
	.endr

	.bss
	.align 4096
	.space 16384
boot_stack_top:
