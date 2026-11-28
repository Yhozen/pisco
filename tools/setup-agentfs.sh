#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# setup-agentfs.sh: what a machine's state file needs (web/state.mjs, docs/linux.md#snapshots), outside the repository,
# in out/npm: the AgentFS SDK (agentfs-sdk, MIT) and Turso's engine for Node (@tursodatabase/database and, for Turso's
# cloud, @tursodatabase/sync: MIT), and for
# the page one module of both on Turso's wasm build, OPFS underneath (out/npm/agentfs-browser.js; with its sync
# build, for Turso's cloud: agentfs-sync-browser.js), served by tools/serve.mjs below /npm/.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); out=$root/out/npm; mkdir -p "$out"; cd "$out"
[ -f package.json ] || echo '{ "name": "pisco-agentfs", "private": true, "type": "module" }' > package.json
npm install --no-audit --no-fund --silent agentfs-sdk@0.6.4 @tursodatabase/database-wasm@0.8.4 @tursodatabase/sync@0.8.4 @tursodatabase/sync-wasm@0.8.4 esbuild@0.25.12
printf "export { AgentFS } from 'agentfs-sdk';\nexport { connect } from '@tursodatabase/database-wasm/bundle';\n" > entry-browser.js
printf "export { AgentFS } from 'agentfs-sdk';\nexport { connect } from '@tursodatabase/sync-wasm/bundle';\n" > entry-sync.js
for e in browser sync; do
  out_js=$([ $e = browser ] && echo agentfs-browser.js || echo agentfs-sync-browser.js)
  node_modules/.bin/esbuild entry-$e.js --bundle --format=esm --platform=browser --conditions=browser --outfile=$out_js --log-level=warning
done
echo "out/npm: agentfs-sdk $(node -p "require('./node_modules/agentfs-sdk/package.json').version"), the page's bundle $(du -h agentfs-browser.js | cut -f1)"
