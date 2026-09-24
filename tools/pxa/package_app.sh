#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/pxa/package_app.sh <app-id> <output-root> [board]

Build one signed PXA App without configuring or building ESP-IDF firmware.
The output root receives pxa-<app-id>/, pxa-<app-id>.pxa and its provenance.

Environment:
  PXSYS_ROOT             Imported pxa-system checkout (default: deps/pxa-system)
  PXA_APP_SOURCE_ROOT    External App source root; it must contain <app-id>/
  PXA_SIGNING_KEY        Signing key accepted by the imported package tool
  PXA_PACKAGE_TARGET     Package target; esp32s3, esp32s31, or simulator (default: board target)
EOF
}

if [[ $# -lt 2 || $# -gt 3 ]]; then
  usage >&2
  exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../.." && pwd)"
app_id="$1"
output_root="$(realpath -m -- "$2")"
board="${3:-${PXA_BOARD:-pai-touch}}"
board_metadata="$project_root/firmware/boards/$board/board.cmake"
pxsys_root="$(realpath -m -- "${PXSYS_ROOT:-$project_root/deps/pxa-system}")"
package_tool="$pxsys_root/tools/package/package_app.sh"

if [[ ! "$app_id" =~ ^[a-z][a-z0-9._-]{0,63}$ ]]; then
  echo "Invalid PXA App id: $app_id" >&2
  exit 2
fi
if [[ ! -f "$board_metadata" ]]; then
  echo "Unknown board profile: $board" >&2
  exit 2
fi
if [[ ! -x "$package_tool" ]]; then
  echo "PXA package tool is unavailable: $package_tool" >&2
  exit 1
fi

board_target="$(sed -nE 's/^[[:space:]]*set\(PXA_BOARD_TARGET[[:space:]]+"([^"]+)"\).*/\1/p' \
  "$board_metadata" | head -n 1)"
package_target="${PXA_PACKAGE_TARGET:-$board_target}"
if [[ "$package_target" != "esp32s3" && "$package_target" != "esp32s31" && "$package_target" != "simulator" ]]; then
  echo "PXA package target is unsupported: $package_target" >&2
  echo "Set PXA_PACKAGE_TARGET to esp32s3, esp32s31, or simulator." >&2
  exit 2
fi

mkdir -p "$output_root"
package_dir="$output_root/pxa-$app_id"
container_output="${PXA_CONTAINER_OUTPUT:-$output_root/pxa-$app_id.pxa}"
provenance_output="${PXA_PROVENANCE_OUTPUT:-$container_output.provenance.json}"

PXA_APP_SOURCE_ROOT="${PXA_APP_SOURCE_ROOT:-$pxsys_root/apps/pxa}" \
PXA_SIGNING_KEY="${PXA_SIGNING_KEY:-$pxsys_root/apps/pxa/.dev-signing/publisher-private.pem}" \
PXA_PACKAGE_OUTPUT_ROOT="$output_root" \
PXA_CONTAINER_OUTPUT="$container_output" \
PXA_PROVENANCE_OUTPUT="$provenance_output" \
"$package_tool" "$app_id" "$package_target" "$package_dir"

printf 'Package directory: %s\nContainer for pxadb: %s\n' "$package_dir" "$container_output"
