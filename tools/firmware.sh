#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/firmware.sh <board> <idf.py action> [action arguments...]

Each board uses build/firmware/<board>/ and is configured with PXA_BOARD.
`--port`/`-p` and `--baud`/`-b` may appear after the action and are forwarded
as idf.py global options. Set IDF_PY to use another idf.py executable.

Examples:
  tools/firmware.sh pai-touch build
  tools/firmware.sh pai-touch flash --port /dev/ttyACM0 --baud 406800
  tools/firmware.sh pai-touch monitor --port /dev/ttyACM0 --baud 115200
EOF
}

if [[ $# -lt 2 ]]; then
  usage >&2
  exit 2
fi

board="$1"
action="$2"
shift 2
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
firmware_dir="$project_root/firmware"
board_dir="$firmware_dir/boards/$board"
build_dir="$project_root/build/firmware/$board"
idf_py="${IDF_PY:-idf.py}"

if [[ ! -f "$board_dir/board.cmake" ]]; then
  echo "Unknown board profile: $board" >&2
  exit 2
fi

global_args=()
action_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --port|-p|--baud|-b)
      [[ $# -ge 2 ]] || {
        echo "Missing value for $1" >&2
        exit 2
      }
      global_args+=("$1" "$2")
      shift 2
      ;;
    *)
      action_args+=("$1")
      shift
      ;;
  esac
done

exec "$idf_py" -C "$firmware_dir" -B "$build_dir" \
  -DPXA_BOARD="$board" "${global_args[@]}" "$action" "${action_args[@]}"
