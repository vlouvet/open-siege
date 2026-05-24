#!/usr/bin/env bash
# Cross-OS connectivity probe — track 27 spec 05.
#
# Runs net-test-client --template-paste from the named client OS(es)
# against a given server address. Classifies the result and prints one
# line per client.
#
# Usage:
#   scripts/net-probe.sh <server-ip> [client-os ...]
#
# If no client-os listed, probes from every host in fleet_all_oses
# EXCEPT one matching <server-ip> (so we don't probe a server from
# itself by accident).
#
# Env:
#   PROBE_TIMEOUT  seconds to wait for ghosts (default 3)
#   PROBE_PORT     server port (default from [default].port in fleet.toml)

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/fleet_lib.sh"

[ $# -ge 1 ] || { echo "usage: $0 <server-ip> [client-os ...]" >&2; exit 2; }
SERVER_IP="$1"; shift
TIMEOUT="${PROBE_TIMEOUT:-3}"
PORT="${PROBE_PORT:-$(fleet_default_field port || echo 28000)}"

if [ $# -gt 0 ]; then
    TARGETS=("$@")
else
    TARGETS=()
    for os in $(fleet_all_oses); do
        os_ip=$(fleet_host_field "$os" ip 2>/dev/null || true)
        [ "$os_ip" = "$SERVER_IP" ] && continue
        TARGETS+=("$os")
    done
fi

exit_code=0
for os in "${TARGETS[@]}"; do
    repo=$(fleet_host_field "$os" repo_path 2>/dev/null)
    [ -z "$repo" ] && { printf "[probe-%-8s] SKIP (no repo_path in fleet.toml)\n" "$os"; continue; }
    bin="$repo/open-siege/build/examples/net-test-client/net-test-client"
    [ "$os" = "windows" ] && bin="${bin}.exe"

    out=""
    out=$(fleet_ssh "$os" -- "$bin --template-paste --host $SERVER_IP --port $PORT --listen-seconds $TIMEOUT 2>&1" || true)

    if echo "$out" | grep -qE 'phase-3: [1-9][0-9]* ghost packets'; then
        n=$(echo "$out" | grep -oE 'phase-3: [0-9]+ ghost packets' | head -1)
        bytes=$(echo "$out" | grep -oE '[0-9]+ bytes total' | head -1)
        printf "[probe-%-8s] OK     (%s, %s in %ss)\n" "$os" "$n" "$bytes" "$TIMEOUT"
    elif echo "$out" | grep -q "phase-1: AcceptConnect received"; then
        printf "[probe-%-8s] HANDSHAKE_ONLY (no ghost burst within %ss)\n" "$os" "$TIMEOUT"
        exit_code=1
    elif echo "$out" | grep -q "phase-1: no AcceptConnect"; then
        printf "[probe-%-8s] NO_REPLY (no UDP response within 3s)\n" "$os"
        exit_code=1
    else
        printf "[probe-%-8s] UNKNOWN — first 3 lines of net-test-client output:\n" "$os"
        echo "$out" | head -3 | sed 's/^/    /'
        exit_code=1
    fi
done

exit "$exit_code"
