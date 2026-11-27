---
title: Calling Linux KVM from a Wasm module
---
An experimental Wasmtime host lets a Wasm module call an x86-64 function through Linux KVM. The module also carries PISCO's lift of the same function. `tests/run.sh kvm` compares both paths with the FNV benchmark checksum and checks that each hardware call returns through the expected halt instruction. [Run the probe](https://github.com/Yhozen/pisco/blob/main/docs/hosts.md#a-wasmtime-host-with-a-kvm-import)
