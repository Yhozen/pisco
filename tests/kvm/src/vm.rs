// SPDX-License-Identifier: GPL-2.0-or-later
use kvm_bindings::{
    kvm_regs, kvm_segment, kvm_userspace_memory_region, KVM_API_VERSION, KVM_MAX_CPUID_ENTRIES,
};
use kvm_ioctls::{Kvm, VcpuExit};
use memmap2::MmapMut;
use std::io::{Error, ErrorKind};
use wasmtime::error::Context;
use wasmtime::{bail, ensure, Result};

const PML4: usize = 0x1000;
const PDPT: usize = 0x2000;
const PD: usize = 0x3000;
const HALT: usize = 0x4000;
const STACK: usize = 0x8000;
const TEXT: usize = 0x10000; // run.sh links the x86 section here
const HUGE_PAGE: usize = 2 << 20;
const INPUT: usize = HUGE_PAGE;

fn put_u64(ram: &mut [u8], addr: usize, value: u64) {
    ram[addr..addr + 8].copy_from_slice(&value.to_le_bytes());
}

pub fn run(kvm: &Kvm, text: &[u8], entry: u32, input: &[u8]) -> Result<u64> {
    ensure!(
        kvm.get_api_version() == KVM_API_VERSION as i32,
        "unsupported KVM API version"
    );
    ensure!(text.len() <= INPUT - TEXT, "text overlaps input");
    ensure!(
        (TEXT..TEXT + text.len()).contains(&(entry as usize)),
        "entry outside text"
    );
    ensure!(input.len() <= 64 << 20, "input exceeds 64 MiB");

    // Identity-map guest RAM with 2 MiB pages. Only this private mapping is visible to KVM.
    let size = (INPUT + input.len()).next_multiple_of(HUGE_PAGE);
    let mut ram = MmapMut::map_anon(size)?;
    put_u64(&mut ram, PML4, PDPT as u64 | 3);
    put_u64(&mut ram, PDPT, PD as u64 | 3);
    for page in 0..size / HUGE_PAGE {
        put_u64(&mut ram, PD + page * 8, (page * HUGE_PAGE) as u64 | 0x83);
    }
    ram[TEXT..TEXT + text.len()].copy_from_slice(text);
    ram[INPUT..INPUT + input.len()].copy_from_slice(input);
    ram[HALT] = 0xf4; // ret from the ELF entry lands on hlt
    put_u64(&mut ram, STACK - 8, HALT as u64);

    let vm = kvm.create_vm().context("KVM_CREATE_VM")?;
    vm.set_tss_address(0xfffbd000)?;
    // SAFETY: ram is page-aligned, never resized or accessed during KVM_RUN, and drops after vm and vcpu.
    unsafe {
        vm.set_user_memory_region(kvm_userspace_memory_region {
            slot: 0,
            guest_phys_addr: 0,
            memory_size: size as u64,
            userspace_addr: ram.as_ptr() as u64,
            flags: 0,
        })?;
    }
    let mut vcpu = vm.create_vcpu(0)?;
    vcpu.set_cpuid2(&kvm.get_supported_cpuid(KVM_MAX_CPUID_ENTRIES)?)?;

    let code = kvm_segment {
        base: 0,
        limit: u32::MAX,
        selector: 8,
        type_: 11,
        present: 1,
        s: 1,
        l: 1,
        g: 1,
        ..Default::default()
    };
    let data = kvm_segment {
        selector: 16,
        type_: 3,
        l: 0,
        db: 1,
        ..code
    };
    let mut sregs = vcpu.get_sregs()?;
    sregs.cs = code;
    sregs.ds = data;
    sregs.es = data;
    sregs.ss = data;
    sregs.fs = data;
    sregs.gs = data;
    sregs.cr3 = PML4 as u64;
    sregs.cr4 |= 1 << 5; // PAE
    sregs.efer |= (1 << 8) | (1 << 10); // LME, LMA
    sregs.cr0 |= 1 | (1 << 31); // PE, PG
    vcpu.set_sregs(&sregs)?;
    vcpu.set_regs(&kvm_regs {
        rip: entry as u64,
        rsp: (STACK - 8) as u64,
        rflags: 2,
        rdi: INPUT as u64,
        rsi: input.len() as u64,
        ..Default::default()
    })?;

    loop {
        match vcpu.run() {
            Ok(VcpuExit::Hlt) => break,
            Ok(other) => bail!("unexpected KVM exit {other:?}"),
            Err(e) if Error::from(e).kind() == ErrorKind::Interrupted => continue,
            Err(e) => return Err(e).context("KVM_RUN"),
        }
    }
    let regs = vcpu.get_regs()?;
    ensure!(
        regs.rip == (HALT + 1) as u64,
        "unexpected HLT at RIP {:#x}",
        regs.rip
    );
    Ok(regs.rax)
}
