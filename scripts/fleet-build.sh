#!/usr/bin/env bash
# Parallel fleet build orchestrator — track 27 spec 02.
#
# `git pull && cmake build` on every host in fleet.toml in parallel.
# Skip unreachable hosts gracefully and print a one-line per-host
# summary at the end.
#
# Usage:
#   scripts/fleet-build.sh                 # current branch of $PWD
#   scripts/fleet-build.sh main            # build the main branch
#   FLEET_SEQ=1 scripts/fleet-build.sh     # sequential mode (for debugging)
#
# Per-host logs: logs/fleet-build/<os>-<timestamp>.log (gitignored)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/fleet_lib.sh"

REF="${1:-$(git -C "$(dirname "$SCRIPT_DIR")" rev-parse --abbrev-ref HEAD)}"
LOG_DIR="$(dirname "$SCRIPT_DIR")/logs/fleet-build"
mkdir -p "$LOG_DIR"
STAMP=$(date +%Y%m%dT%H%M%S)
ONE_SCRIPT="$SCRIPT_DIR/fleet-build-one.sh"

build_one() {
    local os="$1" log="$2"
    local repo ssh_user ssh_host
    repo=$(fleet_host_field "$os" repo_path)
    [ -z "$repo" ] && { echo "[fleet-build $os] missing repo_path in fleet.toml" >"$log"; return 2; }

    if fleet_is_local "$os"; then
        echo "[fleet-build $os] LOCAL build" >"$log"
        bash "$ONE_SCRIPT" "$repo" "$REF" "$os" >>"$log" 2>&1
    else
        ssh_user=$(fleet_host_field "$os" ssh_user)
        ssh_host=$(fleet_host_field "$os" ssh_host)
        [ -z "$ssh_user" ] || [ -z "$ssh_host" ] && {
            echo "[fleet-build $os] missing ssh_user/ssh_host" >"$log"; return 2;
        }
        # Probe SSH reachability first so unreachable hosts fail fast.
        if ! ssh -o BatchMode=yes -o ConnectTimeout=4 \
                "$ssh_user@$ssh_host" true 2>>"$log"; then
            echo "[fleet-build $os] SSH probe failed — skipping" >>"$log"
            return 3
        fi
        echo "[fleet-build $os] REMOTE build via $ssh_user@$ssh_host" >>"$log"
        # Ship the per-host script fresh each time so we can iterate
        # without per-box pulls.
        scp -q "$ONE_SCRIPT" "$ssh_user@$ssh_host:/tmp/fleet-build-one.sh" >>"$log" 2>&1
        ssh "$ssh_user@$ssh_host" \
            "bash /tmp/fleet-build-one.sh '$repo' '$REF' '$os'" >>"$log" 2>&1
    fi
}

# macOS default bash is 3.2 — no associative arrays. Use parallel
# indexed arrays keyed by position.
pids=()
oses=()
logs=()

echo "fleet-build: ref=$REF, log dir=$LOG_DIR"

for os in $(fleet_all_oses); do
    log="$LOG_DIR/${os}-${STAMP}.log"
    if [ "${FLEET_SEQ:-0}" = "1" ]; then
        echo "[fleet-build $os] starting (sequential)..."
        if build_one "$os" "$log"; then
            echo "[fleet-build $os] OK"
        else
            rc=$?
            case $rc in
                3) echo "[fleet-build $os] UNREACHABLE — see $log" ;;
                *) echo "[fleet-build $os] FAILED (exit $rc) — see $log" ;;
            esac
        fi
    else
        build_one "$os" "$log" &
        pids+=("$!")
        oses+=("$os")
        logs+=("$log")
        echo "[fleet-build $os] spawned pid $! -> $log"
    fi
done

if [ "${FLEET_SEQ:-0}" != "1" ]; then
    fail=0; unreachable=0; ok=0
    for i in "${!pids[@]}"; do
        pid="${pids[$i]}"
        os="${oses[$i]}"
        log="${logs[$i]}"
        if wait "$pid"; then
            echo "[fleet-build $os] OK"
            ok=$((ok + 1))
        else
            rc=$?
            case $rc in
                3)  echo "[fleet-build $os] UNREACHABLE — see $log"
                    unreachable=$((unreachable + 1)) ;;
                *)  echo "[fleet-build $os] FAILED (exit $rc) — see $log"
                    fail=$((fail + 1)) ;;
            esac
        fi
    done
    echo "fleet-build: $ok ok, $unreachable unreachable, $fail failed"
    [ "$fail" -gt 0 ] && exit 1
fi
