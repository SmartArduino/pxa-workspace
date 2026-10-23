#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/firmware.sh <board> <idf.py action> [action arguments...]

Each board uses build/firmware/<board>/ and is configured with PXA_BOARD.
`--port`/`-p`, `--baud`/`-b` and `-D` CMake definitions may appear after the
action and are forwarded as idf.py global options. Set IDF_PY to use another
idf.py executable.

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
build_dir="${PXA_FIRMWARE_BUILD_ROOT:-$project_root/build/firmware}/$board"
idf_py="${IDF_PY:-idf.py}"

if [[ ! -f "$board_dir/board.cmake" ]]; then
  echo "Unknown board profile: $board" >&2
  exit 2
fi

global_args=()
action_args=()
flash_port="${ESPPORT:-}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    -D)
      [[ $# -ge 2 ]] || { echo "Missing value for -D" >&2; exit 2; }
      global_args+=("$1" "$2")
      shift 2
      ;;
    -D*)
      global_args+=("$1")
      shift
      ;;
    --port|-p|--baud|-b)
      [[ $# -ge 2 ]] || {
        echo "Missing value for $1" >&2
        exit 2
      }
      global_args+=("$1" "$2")
      if [[ "$1" == --port || "$1" == -p ]]; then flash_port="$2"; fi
      shift 2
      ;;
    *)
      action_args+=("$1")
      shift
      ;;
  esac
done

# Release/CI builds can use a fresh tree and sdkconfig without replacing a
# developer's board configuration or reusing local experimental flags.
if [[ -n "${PXA_FIRMWARE_BUILD_ROOT:-}" ]]; then
  build_dir="$(realpath -m -- "$build_dir")"
  global_args+=("-DSDKCONFIG=$build_dir/sdkconfig")
fi

# Pin Linux USB runtime PM before waiting for the shared build lock or building.
# A manually entered ROM may otherwise autosuspend while CMake/Ninja is busy.
# The guard never opens CDC and follows resets at the same physical USB port.
if [[ "$board" == esp-mosaico && -n "$flash_port" &&
      "${PXA_USB_GUARD_ACTIVE:-}" != 1 ]]; then
  case "$action" in
    flash|app-flash|bootloader-flash|partition-table-flash)
      exec python3 "$script_dir/usb_keep_awake.py" --port "$flash_port" -- \
        "$script_dir/firmware.sh" "$board" "$action" "${global_args[@]}" "${action_args[@]}"
      ;;
  esac
fi

# Recent releases can appear in the official API before the storage index.
# Honor an explicitly configured wrapper, including for custom registries.
export IDF_COMPONENT_WRAPPER="${IDF_COMPONENT_WRAPPER:-$script_dir/idf_component_registry.py}"

# Board build trees are independent, but ESP-IDF's managed_components directory
# is shared. Serialize configuration/build actions so one board cannot replace
# another board's dependencies while its compiler is reading them.
if [[ "$action" != monitor ]]; then
  mkdir -p "$project_root/.pxa"
  exec 9>"$project_root/.pxa/firmware-build.lock"
  flock 9
fi

# Mosaico's external UART supports 2 Mbaud. Explicit --baud still wins, and
# ESPBAUD remains available for users whose adapter needs a slower rate.
if [[ "$board" == esp-mosaico ]]; then
  case "$action" in
    flash|app-flash|bootloader-flash|partition-table-flash)
      export ESPBAUD="${ESPBAUD:-2000000}" ;;
  esac
fi

# A different board can replace shared managed components while this board's
# Ninja cache still names the old files. Refresh the graph on board switches,
# under the same lock; repeated builds of one board keep the fast path.
dependency_state="$project_root/.pxa/firmware-components-board"
case "$action" in
  monitor|clean|fullclean|erase-flash) ;;
  *)
    previous_board=""
    [[ ! -f "$dependency_state" ]] || previous_board="$(cat "$dependency_state")"
    if [[ "$previous_board" != "$board" || "$action" == reconfigure ]]; then
      "$idf_py" -C "$firmware_dir" -B "$build_dir" \
        -DPXA_BOARD="$board" "${global_args[@]}" reconfigure
      printf '%s\n' "$board" > "$dependency_state.tmp"
      mv "$dependency_state.tmp" "$dependency_state"
      [[ "$action" != reconfigure ]] || exit 0
    fi
    ;;
esac

exec "$idf_py" -C "$firmware_dir" -B "$build_dir" \
  -DPXA_BOARD="$board" "${global_args[@]}" "$action" "${action_args[@]}"
