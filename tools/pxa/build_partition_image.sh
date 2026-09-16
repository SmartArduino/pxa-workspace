#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/pxa/build_partition_image.sh [options] <board> [app-id ...]

Build a factory LittleFS image containing one signed PXA App. This is an
explicit factory-provisioning operation; it does not configure, build or flash
ESP-IDF firmware.

Environment:
  PXSYS_ROOT                       Imported pxa-system checkout
  PXA_APP_SOURCE_ROOT              App source root
  --app-source <app-id>=<root>     Source root for one App; it must contain <app-id>/
  PXA_SIGNING_KEY                  Signing key accepted by the package tool
  PXA_STORAGE_PARTITION_LABEL      Override the board's PXA partition label
  PXA_BUILTIN_PACKAGE_ROOT         Override the board's factory package root
  PXA_LITTLEFS_PYTHON              Existing littlefs-python executable
  PXA_LITTLEFS_VENV                Tool virtual environment when installation is needed
  PXA_SYSTEM_FONT                  Font copied with --include-system-font
EOF
}

include_system_font=0
output_image=""
declare -A app_source_roots=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --include-system-font)
      include_system_font=1
      shift
      ;;
    --output)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      output_image="$2"
      shift 2
      ;;
    --app-source)
      [[ $# -ge 2 && "$2" == *=* ]] || { usage >&2; exit 2; }
      app_id="${2%%=*}"
      source_root="${2#*=}"
      [[ "$app_id" =~ ^[a-z][a-z0-9._-]{0,63}$ && -n "$source_root" ]] || {
        echo "Invalid App source mapping: $2" >&2
        exit 2
      }
      app_source_roots["$app_id"]="$source_root"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      break
      ;;
  esac
done
if [[ $# -lt 1 ]]; then
  usage >&2
  exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/../.." && pwd)"
board="$1"
shift
app_ids=("$@")
board_dir="$project_root/firmware/boards/$board"
partition_table="$board_dir/partitions.csv"
board_defaults="$board_dir/sdkconfig.defaults"

for app_id in "${app_ids[@]}"; do
  if [[ ! "$app_id" =~ ^[a-z][a-z0-9._-]{0,63}$ ]]; then
    echo "Invalid PXA App id: $app_id" >&2
    exit 2
  fi
done
if [[ ! -f "$partition_table" || ! -f "$board_defaults" ]]; then
  echo "Board profile must provide partitions.csv and sdkconfig.defaults: $board" >&2
  exit 2
fi

config_value() {
  local key="$1"
  local value
  value="$(sed -nE "s/^${key}=\\\"?([^\\\"[:space:]]+)\\\"?$/\\1/p" \
    "$board_defaults" | tail -n 1)"
  printf '%s' "$value"
}

parse_byte_size() {
  local value="$1"
  local number
  case "$value" in
    0[xX][0-9A-Fa-f]*)
      [[ "$value" =~ ^0[xX][0-9A-Fa-f]+$ ]] || return 1
      printf '%s' "$((value))"
      ;;
    [0-9]*)
      [[ "$value" =~ ^[0-9]+$ ]] || return 1
      printf '%s' "$((10#$value))"
      ;;
    *[Kk])
      number="${value%?}"
      [[ "$number" =~ ^[0-9]+$ ]] || return 1
      printf '%s' "$((10#$number * 1024))"
      ;;
    *[Mm])
      number="${value%?}"
      [[ "$number" =~ ^[0-9]+$ ]] || return 1
      printf '%s' "$((10#$number * 1024 * 1024))"
      ;;
    *)
      return 1
      ;;
  esac
}

partition_label="${PXA_STORAGE_PARTITION_LABEL:-$(config_value CONFIG_PXA_STORAGE_PARTITION_LABEL)}"
factory_root="${PXA_BUILTIN_PACKAGE_ROOT:-$(config_value CONFIG_PXA_BUILTIN_PACKAGE_ROOT)}"
name_max="${PXA_LITTLEFS_NAME_MAX:-$(config_value CONFIG_LITTLEFS_OBJ_NAME_LEN)}"
[[ -n "$partition_label" ]] || partition_label="assets"
[[ -n "$factory_root" ]] || factory_root="system/pxa/builtin"
[[ -n "$name_max" ]] || name_max=96
if [[ "$factory_root" == /* || "$factory_root" == *".."* ]]; then
  echo "Invalid factory package root: $factory_root" >&2
  exit 2
fi
if [[ ! "$name_max" =~ ^[0-9]+$ ]]; then
  echo "Invalid LittleFS name limit: $name_max" >&2
  exit 2
fi

partition_row="$(awk -F, -v label="$partition_label" '
  function trim(value) { gsub(/^[[:space:]]+|[[:space:]]+$/, "", value); return value }
  $0 !~ /^[[:space:]]*#/ && trim($1) == label { print trim($4) "\t" trim($5); exit }
' "$partition_table")"
if [[ -z "$partition_row" ]]; then
  echo "PXA storage partition '$partition_label' is not in $partition_table" >&2
  exit 2
fi
IFS=$'\t' read -r partition_offset partition_size <<< "$partition_row"
if ! parse_byte_size "$partition_offset" >/dev/null ||
   ! partition_size_bytes="$(parse_byte_size "$partition_size")"; then
  echo "Invalid offset or size for partition '$partition_label'" >&2
  exit 2
fi

if [[ -z "$output_image" ]]; then
  image_name="${app_ids[0]:-empty}"
  output_image="$project_root/out/pxa-partitions/$board/$partition_label-$image_name.bin"
fi
output_image="$(realpath -m -- "$output_image")"
mkdir -p "$(dirname "$output_image")"

work_dir="$(mktemp -d "${TMPDIR:-/tmp}/pxa-partition-$board.XXXXXX")"
trap 'rm -rf "$work_dir"' EXIT
storage_root="$work_dir/storage"
factory_root_path="$storage_root/$factory_root"
mkdir -p "$factory_root_path"

for app_id in "${app_ids[@]}"; do
  source_root="${app_source_roots[$app_id]:-${PXA_APP_SOURCE_ROOT:-}}"
  if [[ -n "$source_root" ]]; then
    PXA_APP_SOURCE_ROOT="$source_root" \
    PXA_CONTAINER_OUTPUT="$work_dir/pxa-$app_id.pxa" \
    PXA_PROVENANCE_OUTPUT="$work_dir/pxa-$app_id.pxa.provenance.json" \
    "$script_dir/package_app.sh" "$app_id" "$factory_root_path" "$board"
  else
    PXA_CONTAINER_OUTPUT="$work_dir/pxa-$app_id.pxa" \
    PXA_PROVENANCE_OUTPUT="$work_dir/pxa-$app_id.pxa.provenance.json" \
    "$script_dir/package_app.sh" "$app_id" "$factory_root_path" "$board"
  fi
done

if [[ "$include_system_font" -eq 1 ]]; then
  system_font="${PXA_SYSTEM_FONT:-$project_root/factory/base/system/fonts/noto_sans_cjk_common.ttf}"
  if [[ ! -f "$system_font" ]]; then
    echo "System font is unavailable: $system_font" >&2
    exit 1
  fi
  mkdir -p "$storage_root/system/fonts"
  cp "$system_font" "$storage_root/system/fonts/noto_sans_cjk_common.ttf"
fi

if [[ -n "${PXA_LITTLEFS_PYTHON:-}" ]]; then
  if [[ "$PXA_LITTLEFS_PYTHON" == */* ]]; then
    littlefs_python="$PXA_LITTLEFS_PYTHON"
  else
    littlefs_python="$(command -v "$PXA_LITTLEFS_PYTHON" || true)"
  fi
elif command -v littlefs-python >/dev/null 2>&1; then
  littlefs_python="$(command -v littlefs-python)"
else
  python_bin="${PYTHON:-python3}"
  requirements="$project_root/tools/requirements/littlefs-image.txt"
  venv_dir="${PXA_LITTLEFS_VENV:-$project_root/.pxa-tools/littlefs-venv}"
  littlefs_python="$venv_dir/bin/littlefs-python"
  if [[ ! -x "$littlefs_python" ]]; then
    [[ -f "$requirements" ]] || {
      echo "littlefs-python is unavailable and requirements were not found: $requirements" >&2
      exit 1
    }
    "$python_bin" -m venv "$venv_dir"
    "$venv_dir/bin/pip" install -r "$requirements"
  fi
fi
if [[ ! -x "$littlefs_python" ]]; then
  echo "littlefs-python is not executable: $littlefs_python" >&2
  exit 1
fi

"$littlefs_python" create "$storage_root" "$output_image" -v \
  --fs-size="$partition_size_bytes" --name-max="$name_max" --block-size=4096

printf 'PXA storage image: %s\nPartition label: %s\nFlash offset: %s\nPartition size: %s bytes\n' \
  "$output_image" "$partition_label" "$partition_offset" "$partition_size_bytes"
