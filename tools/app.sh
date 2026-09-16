#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/app.sh build <app-id> [--board <board>] [--target <target>] [--source-root <root>] [--output <dir>]

Build a .pxa without configuring firmware. Without --source-root, the App is
resolved from local/apps.toml, which is intentionally ignored by Git.
EOF
}

if [[ $# -lt 2 || "$1" != "build" ]]; then
  usage >&2
  exit 2
fi

app_id="$2"
shift 2
board="${PXA_BOARD:-pai-touch}"
target=""
source_root="${PXA_APP_SOURCE_ROOT:-}"
output_root=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --board) board="$2"; shift 2 ;;
    --target) target="$2"; shift 2 ;;
    --source-root) source_root="$2"; shift 2 ;;
    --output) output_root="$2"; shift 2 ;;
    *) usage >&2; exit 2 ;;
  esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
catalog="$project_root/local/apps.toml"
if [[ -z "$source_root" && -f "$catalog" ]]; then
  source_root="$("${PYTHON:-python3}" - "$catalog" "$app_id" <<'PYTHON'
import sys
try:
    import tomllib
except ModuleNotFoundError as error:
    raise SystemExit("Python 3.11+ is required to read local/apps.toml") from error

with open(sys.argv[1], "rb") as source:
    catalog = tomllib.load(source)
entry = catalog.get("apps", {}).get(sys.argv[2], {})
value = entry.get("source_root") if isinstance(entry, dict) else None
if isinstance(value, str) and value:
    print(value)
PYTHON
)"
fi
if [[ -z "$source_root" ]]; then
  echo "No source root for '$app_id'. Use --source-root or local/apps.toml." >&2
  exit 2
fi
if [[ -z "$output_root" ]]; then
  output_root="$project_root/local/app-output/$board"
fi

if [[ -z "$target" ]]; then
  case "$board" in
    pai-touch) target="esp32s3" ;;
    *)
      echo "No default package target for board '$board'. Use --target." >&2
      exit 2
      ;;
  esac
fi
case "$target" in
  esp32s3|simulator) ;;
  *)
    echo "Unsupported PXA package target: $target (expected esp32s3 or simulator)" >&2
    exit 2
    ;;
esac

PXA_APP_SOURCE_ROOT="$source_root" \
PXA_PACKAGE_TARGET="$target" \
"$script_dir/pxa/package_app.sh" "$app_id" "$output_root" "$board"
