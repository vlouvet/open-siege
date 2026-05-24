#!/usr/bin/env bash
# Cross-OS test scenario launcher — track 27 spec 03 (+ spec 06 pcap).
#
# Brings up open-siege-t1-server on the named host via SSH, prints a
# big copy-paste join banner for every other host in fleet.toml, then
# streams the server log until Ctrl-C. Ctrl-C propagates through the
# SSH pty so the server catches SIGINT and shuts down cleanly.
#
# Optional --pcap fires `tshark` on the server box for the duration of
# the session, saves the .pcapng to a temp file on the server, and
# scp's it back to captures/cross-os/<timestamp>-<server-os>.pcapng
# when the session ends (spec 27/06).
#
# Usage:
#   scripts/run-scenario.sh <server-os> [--mission NAME] [--port N]
#                            [--skip-mission] [--pcap]
#
# Examples:
#   scripts/run-scenario.sh linux                            # default mission
#   scripts/run-scenario.sh macos  --mission 5_CTF
#   scripts/run-scenario.sh linux  --skip-mission --pcap     # with pcap

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/fleet_lib.sh"

usage() {
    cat <<EOF >&2
usage: $0 <server-os> [--mission NAME] [--port N] [--skip-mission]
  server-os   one of: $(fleet_all_oses)
EOF
    exit 2
}

[ $# -ge 1 ] || usage
SERVER_OS="$1"; shift

MISSION="$(fleet_default_field mission || echo "1_Welcome")"
PORT="$(fleet_default_field port || echo 28000)"
SKIP_MISSION=""
PCAP=""

while [ $# -gt 0 ]; do
    case "$1" in
        --mission)       MISSION="$2"; shift 2 ;;
        --port)          PORT="$2"; shift 2 ;;
        --skip-mission)  SKIP_MISSION="--skip-mission"; shift ;;
        --pcap)          PCAP=1; shift ;;
        -h|--help)       usage ;;
        *)               echo "unknown arg: $1" >&2; usage ;;
    esac
done

SERVER_IP="$(fleet_host_field "$SERVER_OS" ip)"
SERVER_USER="$(fleet_host_field "$SERVER_OS" ssh_user)"
SERVER_HOST="$(fleet_host_field "$SERVER_OS" ssh_host)"
SERVER_REPO="$(fleet_host_field "$SERVER_OS" repo_path)"
[ -z "$SERVER_IP" ] || [ -z "$SERVER_USER" ] || [ -z "$SERVER_HOST" ] || [ -z "$SERVER_REPO" ] && {
    echo "fleet.toml missing required field for $SERVER_OS" >&2
    exit 2
}

server_bin_path() {
    local repo="$1" os="$2"
    local bin="$repo/open-siege/build/apps/open-siege-t1-server/open-siege-t1-server"
    [ "$os" = "windows" ] && bin="${bin}.exe"
    printf '%s' "$bin"
}
client_bin_path() {
    local repo="$1" os="$2"
    local bin="$repo/open-siege/build/apps/open-siege-t1-client/open-siege-t1-client"
    [ "$os" = "windows" ] && bin="${bin}.exe"
    printf '%s' "$bin"
}
probe_bin_path() {
    local repo="$1" os="$2"
    local bin="$repo/open-siege/build/examples/net-test-client/net-test-client"
    [ "$os" = "windows" ] && bin="${bin}.exe"
    printf '%s' "$bin"
}

SERVER_BIN="$(server_bin_path "$SERVER_REPO" "$SERVER_OS")"

# Build the server arg list. --tribes-dir defaults to $repo/tribes-game.
SERVER_ARGS=( --port "$PORT" --tick-hz 32 )
if [ -n "$SKIP_MISSION" ]; then
    SERVER_ARGS+=( --skip-mission )
else
    SERVER_ARGS+=( --mission "$MISSION" --tribes-dir "$SERVER_REPO/tribes-game" )
fi

MISSION_LABEL="$MISSION"
[ -n "$SKIP_MISSION" ] && MISSION_LABEL="(none, --skip-mission)"
cat <<EOF
================================================================
  SERVER: $SERVER_OS ($SERVER_IP)  port $PORT  mission $MISSION_LABEL
================================================================
EOF
for os in $(fleet_all_oses); do
    [ "$os" = "$SERVER_OS" ] && continue
    repo="$(fleet_host_field "$os" ip 2>/dev/null && fleet_host_field "$os" repo_path)" || true
    repo="$(fleet_host_field "$os" repo_path)"
    [ -z "$repo" ] && continue
    cat <<EOF
  Probe from $os:
    $(probe_bin_path "$repo" "$os") --template-paste --host $SERVER_IP --port $PORT --listen-seconds 3
  Join from $os:
    $(client_bin_path "$repo" "$os") --server $SERVER_IP:$PORT
EOF
done
cat <<EOF
================================================================
  Server log streams below. Ctrl-C to shut down.
================================================================
EOF

# Spec 27/06: optional pcap. tshark backgrounds on the server box and
# writes to a remote tmpfile; we scp it back on cleanup. tshark needs
# CAP_NET_RAW or wireshark-group membership on the capture host; if
# tshark isn't installed we warn but don't fail the scenario.
LOCAL_PCAP=""
REMOTE_PCAP=""
if [ -n "$PCAP" ]; then
    PCAP_STAMP=$(date +%Y%m%dT%H%M%S)
    REMOTE_PCAP="/tmp/open-siege-${PCAP_STAMP}-${SERVER_OS}.pcapng"
    LOCAL_PCAP_DIR="$(cd "$(dirname "$SCRIPT_DIR")" && pwd)/../captures/cross-os"
    mkdir -p "$LOCAL_PCAP_DIR"
    LOCAL_PCAP="$LOCAL_PCAP_DIR/${PCAP_STAMP}-${SERVER_OS}.pcapng"
    # Probe + spawn tshark on the server box.
    if fleet_is_local "$SERVER_OS"; then
        if command -v tshark >/dev/null 2>&1; then
            ( tshark -i any -f "udp port $PORT" -w "$REMOTE_PCAP" \
                >/dev/null 2>&1 ) &
            TSHARK_PID=$!
            echo "[pcap] local tshark started pid=$TSHARK_PID writing $REMOTE_PCAP"
        else
            echo "[pcap] tshark not found locally — skipping capture" >&2
            PCAP=""
        fi
    else
        if ssh -o BatchMode=yes "$SERVER_USER@$SERVER_HOST" \
                "command -v tshark >/dev/null" 2>/dev/null; then
            ssh -o BatchMode=yes "$SERVER_USER@$SERVER_HOST" \
                "nohup tshark -i any -f 'udp port $PORT' -w '$REMOTE_PCAP' >/dev/null 2>&1 & echo \$! > /tmp/open-siege-tshark.pid"
            echo "[pcap] remote tshark spawned on $SERVER_OS writing $REMOTE_PCAP"
        else
            echo "[pcap] tshark not found on $SERVER_OS — skipping capture" >&2
            PCAP=""
        fi
    fi
fi

cleanup_pcap() {
    [ -z "$PCAP" ] && return 0
    echo
    echo "[pcap] stopping capture + retrieving pcapng..."
    if fleet_is_local "$SERVER_OS"; then
        [ -n "${TSHARK_PID:-}" ] && kill -TERM "$TSHARK_PID" 2>/dev/null || true
        # Local already lives where we want it; just move into place.
        [ -f "$REMOTE_PCAP" ] && mv "$REMOTE_PCAP" "$LOCAL_PCAP"
    else
        ssh -o BatchMode=yes "$SERVER_USER@$SERVER_HOST" \
            "[ -f /tmp/open-siege-tshark.pid ] && kill -TERM \$(cat /tmp/open-siege-tshark.pid) 2>/dev/null; rm -f /tmp/open-siege-tshark.pid"
        sleep 0.5  # let tshark flush
        scp -q "$SERVER_USER@$SERVER_HOST:$REMOTE_PCAP" "$LOCAL_PCAP" 2>/dev/null || \
            echo "[pcap] scp of $REMOTE_PCAP failed — file may be on $SERVER_OS at $REMOTE_PCAP" >&2
        ssh -o BatchMode=yes "$SERVER_USER@$SERVER_HOST" "rm -f '$REMOTE_PCAP'" 2>/dev/null || true
    fi
    if [ -f "$LOCAL_PCAP" ]; then
        echo "[pcap] saved $(du -h "$LOCAL_PCAP" | cut -f1) to $LOCAL_PCAP"
    fi
}
trap cleanup_pcap EXIT INT TERM

# Foreground ssh -t: the local pty's SIGINT is forwarded to the remote
# pty's foreground process. `exec` on the remote side replaces the
# remote bash with the server binary so SIGINT goes straight to our
# server (which already handles it via on_sigint -> g_quit.store(true)
# -> clean stop()).
if fleet_is_local "$SERVER_OS"; then
    "$SERVER_BIN" "${SERVER_ARGS[@]}"
else
    ssh -t "$SERVER_USER@$SERVER_HOST" \
        "exec '$SERVER_BIN' ${SERVER_ARGS[*]}"
fi
# Note: dropped `exec` so the trap runs on normal exit too.
