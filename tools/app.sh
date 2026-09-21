#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/app.sh build <app-id> [--board <board>] [--target <target>[,<target>...]]
                           [--source-root <root>] [--output <dir>]

Build a signed .pxa without configuring firmware. Without --source-root, the
App is resolved from local/apps.toml, which is intentionally ignored by Git.

Targets (comma separated, or 'all'):
  esp32s3    Xtensa ESP32-S3 AOT            artifacts/main.esp32-s3.aot
  esp32s31   RISC-V32 ILP32F ESP32-S31 AOT  artifacts/main.esp32-s31.aot
  simulator  x86_64 AOT                     artifacts/main.linux-x86_64.aot
  wasm       WebAssembly interpreter build  artifacts/main.wasm

Aliases: xtensa -> esp32s3; riscv32, riscv32-ilp32f -> esp32s31;
         x86, x86_64, linux-x86_64 -> simulator.
'all' expands to wasm,x86,esp32s3,esp32s31.

The first AOT target signs the package and names the primary artifact; the
remaining AOT targets are built into the same package as extra architecture
artifacts. The wasm artifact is included whenever the App declares artifact
mode "both" (the default) or "wasm" in package.json.

Default target: the board's own target (pai-touch -> esp32s3,
esp32s31-korvo-1 -> esp32s31).
Default output: local/app-output/<board> (container: pxa-<app-id>.pxa).
EOF
}

if [[ $# -lt 2 || "$1" != "build" ]]; then
  usage >&2
  exit 2
fi

app_id="$2"
shift 2
board="${PXA_BOARD:-pai-touch}"
target_spec=""
source_root="${PXA_APP_SOURCE_ROOT:-}"
output_root=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --board) board="$2"; shift 2 ;;
    --target) target_spec="$2"; shift 2 ;;
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

targets=()
requested_wasm=0
board_target=""
case "$board" in
  pai-touch) board_target="esp32s3" ;;
  esp32s31-korvo-1) board_target="esp32s31" ;;
esac
if [[ -n "$target_spec" ]]; then
  IFS=',' read -r -a raw_targets <<< "$target_spec"
  for raw_target in "${raw_targets[@]}"; do
    [[ -n "$raw_target" ]] || continue
    case "$raw_target" in
      all)
        case "$board" in
          esp32s31-korvo-1) targets+=(esp32s31 esp32s3 simulator) ;;
          *) targets+=(esp32s3 esp32s31 simulator) ;;
        esac
        requested_wasm=1
        ;;
      wasm) requested_wasm=1 ;;
      esp32s3|xtensa) targets+=(esp32s3) ;;
      esp32s31|esp32-s31|riscv32|riscv32-ilp32f) targets+=(esp32s31) ;;
      simulator|x86|x86_64|linux-x86_64) targets+=(simulator) ;;
      *)
        echo "Unsupported PXA package target: $raw_target" >&2
        usage >&2
        exit 2
        ;;
    esac
  done
fi

if [[ ${#targets[@]} -gt 0 ]]; then
  unique_targets=()
  for candidate in "${targets[@]}"; do
    duplicate=0
    for existing in "${unique_targets[@]}"; do
      if [[ "$existing" == "$candidate" ]]; then
        duplicate=1
        break
      fi
    done
    if [[ "$duplicate" -eq 0 ]]; then
      unique_targets+=("$candidate")
    fi
  done
  targets=("${unique_targets[@]}")
else
  if [[ -z "$board_target" ]]; then
    echo "No default package target for board '$board'. Use --target." >&2
    exit 2
  fi
  targets=("$board_target")
fi
if [[ -n "$board_target" && ${#targets[@]} -gt 1 && "${targets[0]}" != "$board_target" ]]; then
  reordered=()
  for candidate in "${targets[@]}"; do
    [[ "$candidate" == "$board_target" ]] || reordered+=("$candidate")
  done
  targets=("$board_target" "${reordered[@]}")
fi
primary_target="${targets[0]}"

if [[ "$requested_wasm" -eq 1 && ${#targets[@]} -gt 1 ]]; then
  echo "Building architectures: wasm, ${targets[*]}" >&2
fi
if [[ "$target_spec" =~ ^[[:space:]]*wasm[[:space:]]*$ ]]; then
  echo "wasm is included with an AOT target; signing with board default '$primary_target'." >&2
  echo "A wasm-only package requires \"artifact\": \"wasm\" in package.json." >&2
fi

if [[ ${#targets[@]} -eq 1 ]]; then
  PXA_APP_SOURCE_ROOT="$source_root" \
  PXA_PACKAGE_TARGET="$primary_target" \
  "$script_dir/pxa/package_app.sh" "$app_id" "$output_root" "$board"
else
  work_dir="$(mktemp -d "${TMPDIR:-/tmp}/pxa-$app_id-arch.XXXXXX")"
  trap 'rm -rf "$work_dir"' EXIT
  extra_aot_dir="$work_dir/extra-aot"
  mkdir -p "$extra_aot_dir"

  for target in "${targets[@]:1}"; do
    extra_root="$work_dir/out-$target"
    extra_output="$extra_root/pxa-$app_id"
    mkdir -p "$extra_root"
    echo "Building '$app_id' for $target..." >&2
    PXA_APP_SOURCE_ROOT="$source_root" \
    PXA_PACKAGE_TARGET="$target" \
    PXA_PACKAGE_OUTPUT_ROOT="$extra_root" \
    PXA_CONTAINER_OUTPUT="$work_dir/$app_id-$target.pxa" \
    PXA_PROVENANCE_OUTPUT="$work_dir/$app_id-$target.provenance.json" \
    "$script_dir/pxa/package_app.sh" "$app_id" "$extra_output" "$board"
    for artifact in "$extra_output"/artifacts/*.aot; do
      [[ -f "$artifact" ]] || continue
      cp "$artifact" "$extra_aot_dir/"
    done
  done

  echo "Building '$app_id' for $primary_target (primary)..." >&2
  PXA_APP_SOURCE_ROOT="$source_root" \
  PXA_PACKAGE_TARGET="$primary_target" \
  PXA_EXTRA_AOT_DIR="$extra_aot_dir" \
  "$script_dir/pxa/package_app.sh" "$app_id" "$output_root" "$board"
fi

package_dir="$output_root/pxa-$app_id"
if [[ -d "$package_dir/artifacts" ]]; then
  echo "Architecture artifacts in $package_dir/artifacts:" >&2
  (cd "$package_dir/artifacts" && ls -1) >&2
fi
