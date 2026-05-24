# Source me from other cross-OS test scripts. Bash 4+ required.
#
# Usage:
#   source "$(dirname "$0")/fleet_lib.sh"
#   IP=$(fleet_host_field linux ip)
#   fleet_ssh linux -- uname -a

FLEET_FILE="${FLEET_FILE:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fleet.toml}"

# fleet_host_field <os> <field>  -> echoes the value, no trailing newline strip
fleet_host_field() {
    local os="$1" field="$2"
    [ -r "$FLEET_FILE" ] || { echo "fleet_lib: missing $FLEET_FILE (copy fleet.toml.example?)" >&2; return 1; }
    awk -v section="[host.$os]" -v key="$field" '
        $0 == section          { in_section=1; next }
        /^\[/                  { in_section=0 }
        in_section && $1 == key {
            sub(/^[^=]*=[ \t]*/, "")
            gsub(/^"|"$/, "")
            gsub(/^[ \t]+|[ \t]+$/, "")
            print
            exit
        }
    ' "$FLEET_FILE"
}

# fleet_default_field <field> -> reads from [default] block
fleet_default_field() {
    local field="$1"
    awk -v key="$field" '
        $0 == "[default]"      { in_section=1; next }
        /^\[/                  { in_section=0 }
        in_section && $1 == key {
            sub(/^[^=]*=[ \t]*/, "")
            gsub(/^"|"$/, "")
            gsub(/^[ \t]+|[ \t]+$/, "")
            print
            exit
        }
    ' "$FLEET_FILE"
}

fleet_all_oses() { echo macos windows linux; }

# fleet_ssh <os> [--] <cmd...>  -> runs cmd over ssh with batch-mode + 5s timeout
fleet_ssh() {
    local os="$1"; shift
    [ "${1:-}" = "--" ] && shift
    local user host
    user=$(fleet_host_field "$os" ssh_user)
    host=$(fleet_host_field "$os" ssh_host)
    [ -z "$user" ] || [ -z "$host" ] && { echo "fleet_lib: no ssh creds for $os" >&2; return 1; }
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$user@$host" "$@"
}
