# Notes for agents

Start with [docs/plan.md](docs/plan.md) (the parts and the rules a change follows) and [docs/status.md](docs/status.md).

## Troubleshooting

- **Hitting a browser performance wall?** Profile first (a Chrome Performance trace of the title's worker): the case
  in [docs/hosts.md](docs/hosts.md) looked like the browser and was ours. If it really is the browser (WebGPU's
  features, memory, the GPU process), that page has the alternatives weighed and the proposed fix: a native Rust host
  (wasmtime + wgpu) next to the browser.
