// SPDX-License-Identifier: GPL-2.0-or-later
use kvm_ioctls::Kvm;
use wasmtime::error::Context;
use wasmtime::{bail, ensure, Caller, Engine, Extern, Linker, Module, Result, Store};

mod vm;

struct Host {
    kvm: Option<Kvm>,
    halts: u32,
}

fn wasm_slice(memory: &[u8], offset: u32, len: u32) -> Result<&[u8]> {
    memory
        .get(offset as usize..)
        .and_then(|tail| tail.get(..len as usize))
        .context("range outside Wasm linear memory")
}

fn run_x86_64(
    mut caller: Caller<'_, Host>,
    text: u32,
    text_len: u32,
    entry: u32,
    input: u32,
    len: u32,
) -> Result<u64> {
    let memory = caller
        .get_export("memory")
        .and_then(Extern::into_memory)
        .context("module must export its wasm32 memory")?;
    let bytes = memory.data(&caller);
    let kvm = caller
        .data()
        .kvm
        .as_ref()
        .context("KVM import called in lifted mode")?;
    let result = vm::run(
        kvm,
        wasm_slice(bytes, text, text_len)?,
        entry,
        wasm_slice(bytes, input, len)?,
    )?;
    caller.data_mut().halts += 1;
    Ok(result)
}

fn main() -> Result<()> {
    let args: Vec<_> = std::env::args().collect();
    let [_, mode, path] = args.as_slice() else {
        bail!("usage: pisco-kvm-probe <lifted|kvm> <probe.wasm>");
    };
    let kvm = match mode.as_str() {
        "lifted" => None,
        "kvm" => Some(Kvm::new().context("open /dev/kvm")?),
        _ => bail!("unknown mode {mode}"),
    };
    let engine = Engine::default();
    let module = Module::from_file(&engine, path)?;
    let mut store = Store::new(&engine, Host { kvm, halts: 0 });
    let mut linker = Linker::new(&engine);
    linker.func_wrap("hypervisor", "run_x86_64", run_x86_64)?;
    let instance = linker.instantiate(&mut store, &module)?;
    let check = instance.get_typed_func::<(u32, u32), u64>(&mut store, "check")?;

    // FNV-1a of bytes (i * 131) mod 256. The 64 MiB value is fixtures/bench-native's.
    for (len, expected) in [
        (0, 0xcbf29ce484222325),
        (257, 0x0234d6b8042944df),
        (64 << 20, 0x48e7c13380222325),
    ] {
        let use_kvm = u32::from(mode == "kvm");
        let before = store.data().halts;
        let got = check.call(&mut store, (use_kvm, len))?;
        ensure!(
            store.data().halts - before == use_kvm,
            "{mode}: unexpected KVM exit count"
        );
        ensure!(
            got == expected,
            "{mode}: {len} bytes: expected {expected:016x}, got {got:016x}"
        );
        println!("{mode}: {len} bytes: {got:016x}");
    }
    println!("{mode}: {} KVM_EXIT_HLT returns", store.data().halts);
    Ok(())
}
