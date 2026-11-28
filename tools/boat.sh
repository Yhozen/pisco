#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# boat.sh (experimental, optional): develop on boat.dev sandboxes (docs/dev-machines.md). Bare-metal `large` machines (8 vCPU) lift, compile
# and test several times faster than a small VM, and a saved snapshot with the toolchains, the builds and pisco's
# caches warm makes a new one ready in seconds. Needs the `boat` CLI, signed in (boat login, or BOAT_DEV_API_KEY).
#   boat.sh bake [name]           a new sandbox from nothing: this working tree, the toolchains, the builds
#                                 and the Linux apps' roots (tools/build-*.sh), then a saved snapshot <name>
#                                 (default pisco-dev). Prints how long each part took
#   boat.sh new [name]            a sandbox from the snapshot, with this working tree on it; prints its id
#   boat.sh sync <id>             this working tree (tracked and new files, not ignored ones) onto it, in ~/pisco
#   boat.sh run <id> <command>    sync, then run the command in ~/pisco; its output here, its exit status
#   boat.sh test [group...]       tests/run.sh's steps (default: all but the ones needing a game or an SDK), a group
#                                 of them (steps joined by +) on each sandbox, from the snapshot, all at once (the
#                                 lifter and pisco built again first, from this tree); logs
#                                 in out/boat/<group>.log; the sandboxes are deleted after. (Keep the groups under the
#                                 plan's starts a minute: 12 on the $20 plan)
#   boat.sh rm <id>...            delete sandboxes
# Environment: BOAT_TYPE (default large), BOAT_SNAPSHOT (default pisco-dev).
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"
TYPE=${BOAT_TYPE:-large}; SNAP=${BOAT_SNAPSHOT:-pisco-dev}
command -v boat > /dev/null || { echo "boat.sh: no boat CLI (curl -fsSL https://boat.dev/install | sh)" >&2; exit 2; }
if [ -n "${BOAT_DEV_API_KEY:-}" ] && ! boat whoami > /dev/null 2>&1; then printenv BOAT_DEV_API_KEY | boat login --key-stdin > /dev/null 2>&1; fi

# a command on the sandbox: its output as it comes, its exit status (boat's JSON frames, unwrapped)
remote() { # id, command
  boat exec "$1" --timeout 0 --json -- "export PATH=\$HOME/.cargo/bin:\$PATH; $2" 2> /dev/null | python3 -I -c '
import json, sys
code = 1
for line in sys.stdin:
    try: d = json.loads(line)
    except ValueError: continue
    t = d.get("type")
    if t in ("stdout", "stderr"): (sys.stdout if t == "stdout" else sys.stderr).write(d.get("data", "")); sys.stdout.flush()
    elif t == "exit": code = d.get("exitCode") or 0
    elif "exitCode" in d and "stdout" in d: sys.stdout.write(d["stdout"]); sys.stderr.write(d["stderr"]); code = d["exitCode"] or 0
sys.exit(code)'
}
new() { # [name] [from-snapshot]: the id of a new ready sandbox
  boat new --type "$TYPE" ${2:+--from "$2"} --name "${1:-pisco-dev}" --json 2> /dev/null | python3 -I -c '
import json, sys
for line in sys.stdin:
    try: d = json.loads(line)
    except ValueError: continue
    if d.get("event") == "ready": print(d["id"]); sys.exit(0)
    if d.get("event") == "error": sys.exit("boat new: " + json.dumps(d))
sys.exit("boat new: no ready event")'
}
sync() { # id
  local tgz; tgz=$(mktemp --suffix=.tgz)
  git ls-files -co --exclude-standard -z | tar -czf "$tgz" --null -T - 2> /dev/null
  boat scp "$tgz" "$1:sync.tgz" > /dev/null 2>&1 || { echo "boat.sh: could not copy the working tree to $1" >&2; rm -f "$tgz"; return 1; }
  rm -f "$tgz"
  remote "$1" 'mkdir -p ~/pisco && tar -xzf ~/sync.tgz -C ~/pisco && rm ~/sync.tgz'
}
since() { awk -v s="$1" -v e="$(date +%s.%N)" 'BEGIN { printf "%.1f", e - s }'; }

case "${1:-}" in
  bake)
    name=${2:-$SNAP}; t0=$(date +%s.%N)
    id=$(new "$name-bake") || exit 1
    echo "sandbox $id ready in $(since "$t0") s"
    boat extend "$id" --hours 3 > /dev/null 2>&1
    sync "$id" || exit 1
    remote "$id" "set -e
      T=~/bake-timings.txt; : > \$T
      P() { local n=\$1; shift; local s=\$(date +%s.%N); \"\$@\" > ~/bake-\$n.log 2>&1 || { echo \"\$n failed: ~/bake-\$n.log\"; tail -5 ~/bake-\$n.log; exit 1; }; awk -v n=\"\$n\" -v s=\"\$s\" -v e=\"\$(date +%s.%N)\" 'BEGIN { printf \"  %-16s %7.1f s\\n\", n, e - s }' | tee -a \$T; }
      cd ~/pisco
      P apt sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq lld
      P toolchains tools/setup-toolchain.sh wasi mingw rust naga reuse musl linux-headers
      P cargo cargo build -q --release -p pisco-lifter -p pisco-shaders -p pisco
      P fastfetch tools/build-fastfetch.sh
      P neofetch tools/build-neofetch.sh
      for t in cowsay; do [ -x tools/build-\$t.sh ] && P \$t tools/build-\$t.sh; done
      P tests tests/run.sh build layout node bench win32 linux fastfetch neofetch cowsay" || exit 1
    echo "saving the snapshot $name"
    boat snapshot "$id" "$name" > /dev/null 2>&1 || { echo "boat.sh: the snapshot failed; the sandbox $id is kept" >&2; exit 1; }
    boat delete --yes "$id" > /dev/null 2>&1
    echo "snapshot $name saved ($(since "$t0") s in all)" ;;
  new)
    t0=$(date +%s.%N); id=$(new "${2:-pisco-dev}" "$SNAP") || exit 1
    sync "$id" || exit 1
    echo "$id"; echo "ready with this working tree in $(since "$t0") s" >&2 ;;
  sync) sync "$2" ;;
  run) id=$2; shift 2; sync "$id" && remote "$id" "cd ~/pisco && $*" ;;
  rm) shift; for id in "$@"; do boat delete --yes "$id" > /dev/null 2>&1 && echo "deleted $id"; done ;;
  test)
    shift; steps=${*:-build+layout+node+bench+win32 linux+ffp+sm+gdi images chrome fastfetch neofetch cowsay}
    mkdir -p out/boat; t0=$(date +%s.%N); pids=()
    for s in $steps; do
      ( id=$(new "pisco-test-${s//+/-}" "$SNAP") || { echo "FAIL  $s: no sandbox"; exit 1; }
        ok=1; if sync "$id" > /dev/null && remote "$id" "cd ~/pisco && cargo build -q --release -p pisco-lifter -p pisco -p pisco-shaders && tests/run.sh ${s//+/ }" > "out/boat/$s.log" 2>&1; then ok=0; fi
        boat delete --yes "$id" > /dev/null 2>&1
        grep -E "^(ok|FAIL|skip) " "out/boat/$s.log" | cut -c1-160
        exit $ok ) & pids+=($!)
    done
    fail=0; for p in "${pids[@]}"; do wait "$p" || fail=1; done
    echo "$(echo $steps | wc -w) sandboxes in $(since "$t0") s; logs in out/boat/"; exit $fail ;;
  *) sed -n '3,19p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
