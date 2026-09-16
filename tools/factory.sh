#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/factory.sh image <board> [--profile <file>] [--overlay <file>] [--output <image>]

The tracked profile defines the board's reproducible factory set. An optional
local overlay adds private development Apps without changing tracked files.
Each App source root is resolved from local/apps.toml.
EOF
}

if [[ $# -lt 2 || "$1" != "image" ]]; then
  usage >&2
  exit 2
fi

board="$2"
shift 2
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
profile="$project_root/factory/profiles/$board.toml"
overlay=""
output_image=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile) profile="$2"; shift 2 ;;
    --overlay) overlay="$2"; shift 2 ;;
    --output) output_image="$2"; shift 2 ;;
    *) usage >&2; exit 2 ;;
  esac
done
if [[ ! -f "$profile" ]]; then
  echo "Factory profile is unavailable: $profile" >&2
  exit 2
fi
if [[ -n "$overlay" && ! -f "$overlay" ]]; then
  echo "Factory overlay is unavailable: $overlay" >&2
  exit 2
fi

mapfile -t profile_rows < <("${PYTHON:-python3}" - "$profile" "$overlay" <<'PYTHON'
import sys
try:
    import tomllib
except ModuleNotFoundError as error:
    raise SystemExit("Python 3.11+ is required to read factory profiles") from error

include_font = False
apps = []
seen = set()
for path in sys.argv[1:]:
    if not path:
        continue
    with open(path, "rb") as source:
        document = tomllib.load(source)
    factory = document.get("factory", {})
    if not isinstance(factory, dict):
        raise SystemExit(f"[factory] must be a table: {path}")
    if "include_system_font" in factory:
        include_font = bool(factory["include_system_font"])
    for app in factory.get("apps", []):
        if not isinstance(app, str) or not app:
            raise SystemExit(f"factory.apps must contain non-empty strings: {path}")
        if app not in seen:
            seen.add(app)
            apps.append(app)
print(f"font\t{int(include_font)}")
for app in apps:
    print(f"app\t{app}")
PYTHON
)

include_font=0
apps=()
for row in "${profile_rows[@]}"; do
  IFS=$'\t' read -r kind value <<< "$row"
  case "$kind" in
    font) include_font="$value" ;;
    app) apps+=("$value") ;;
  esac
done

catalog="$project_root/local/apps.toml"
command=("$script_dir/pxa/build_partition_image.sh")
if [[ "$include_font" == "1" ]]; then
  command+=(--include-system-font)
fi
if [[ -n "$output_image" ]]; then
  command+=(--output "$output_image")
fi
if [[ ${#apps[@]} -gt 0 && ! -f "$catalog" ]]; then
  echo "Factory profile needs App sources, but local catalog is unavailable: $catalog" >&2
  exit 2
fi
for app in "${apps[@]}"; do
  source_root="$("${PYTHON:-python3}" - "$catalog" "$app" <<'PYTHON'
import sys
try:
    import tomllib
except ModuleNotFoundError as error:
    raise SystemExit("Python 3.11+ is required to read local/apps.toml") from error

with open(sys.argv[1], "rb") as source:
    catalog = tomllib.load(source)
entry = catalog.get("apps", {}).get(sys.argv[2], {})
value = entry.get("source_root") if isinstance(entry, dict) else None
if not isinstance(value, str) or not value:
    raise SystemExit(f"No source_root configured for App: {sys.argv[2]}")
print(value)
PYTHON
)"
  command+=(--app-source "$app=$source_root")
done
command+=("$board" "${apps[@]}")
exec "${command[@]}"
