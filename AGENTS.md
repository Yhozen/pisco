# Notes for agents

Start with [docs/plan.md](docs/plan.md) (the parts and the rules a change follows) and [docs/status.md](docs/status.md).

Touching a recipe, a base, an image's config or layers, a session, a host function, an `SW_*` variable, or how apps
are installed and published? The contract is [docs/spec/](docs/spec/README.md): read its README, then only the page
it points you to. Change the spec in the same pull request (rule 7); `tests/run.sh spec` checks the lists match the
code.

## Troubleshooting

- **Hitting a browser performance wall?** Profile first (a Chrome Performance trace of the title's worker): the case
  in [docs/hosts.md](docs/hosts.md) looked like the browser and was ours. If it really is the browser (WebGPU's
  features, memory, the GPU process), that page has the alternatives weighed and the proposed fix: a native Rust host
  (wasmtime + wgpu) next to the browser.
