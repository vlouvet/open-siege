#!/usr/bin/env bash
# Cross-OS test scenario launcher — track 27 spec 03.
#
# Brings up open-siege-t1-server on the named host via SSH, prints a
# big copy-paste join banner for every other host in fleet.toml, then
# streams the server log until Ctrl-C. Ctrl-C propagates through the
# SSH pty so the server catches SIGINT and shuts down cleanly.
#
# Usage:
#   scripts/run-scenario.sh <server-os> [--mission NAME] [--port N]
#                            [--skip-mission]
#
# Examples:
#   scripts/run-scenario.sh linux                          # default mission
#   scripts/run-scenario.sh macos  --mission 5_CTF
#   scripts/run-scenario.sh linux  --skip-mission --port 28999

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

while [ $# -gt 0 ]; do
    case "$1" in
        --mission)       MISSION="$2"; shift 2 ;;
        --port)          PORT="$2"; shift 2 ;;
        --skip-mission)  SKIP_MISSION="--skip-mission"; shift ;;
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

# Foreground ssh -t: the local pty's SIGINT is forwarded to the remote
# pty's foreground process. `exec` on the remote side replaces the
# remote bash with the server binary so SIGINT goes straight to our
# server (which already handles it via on_sigint -> g_quit.store(true)
# -> clean stop()).
exec ssh -t "$SERVER_USER@$SERVER_HOST" \
    "exec '$SERVER_BIN' ${SERVER_ARGS[*]}"
