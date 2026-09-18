#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: tools/simulator.sh [ui|product|service] [configure|build|run|start|stop|status] [--profile <name>] [--instance <name>] [--app-root <root>] [--package <dir>|--installed <app-id>] [--state-root <dir>] [--publisher-key <der>] [--listen <host:port|host:auto>] [-- simulator arguments]

Runs either the desktop standard UI or a signed PXA package with a workspace
board profile. Omit the mode, or select `ui`, for the standard-UI simulator.

With no action, the simulator is configured, built, and started. The default
profile is "generic". `run` also starts the profile's PXADB2 service unless
`PXA_SIMULATOR_AUTOSTART_PXADB=0` is set.

Examples:
  tools/simulator.sh ui
  tools/simulator.sh ui --profile pai-touch
  tools/simulator.sh ui build --profile pai-touch
  tools/simulator.sh ui --profile pai-touch -- --launch pxa-weather
  tools/app.sh build arcade --target simulator --source-root deps/pxa-system/apps/pxa
  tools/simulator.sh product --profile pai-touch \
    --package local/app-output/pai-touch/pxa-arcade \
    --publisher-key deps/pxa-system/apps/pxa/.dev-signing/publisher-public.der
  tools/simulator.sh service start --profile pai-touch
  pxadb package install local/app-output/pai-touch/pxa-arcade.pxa --simulator pai-touch
  tools/simulator.sh product --profile pai-touch --installed pxa-arcade
  tools/simulator.sh service start --profile pai-touch --listen 0.0.0.0:auto
  tools/simulator.sh ui --profile pai-touch --instance demo-a --listen 127.0.0.1:auto
EOF
}

mode="ui"
if [[ $# -gt 0 && ( "$1" == "ui" || "$1" == "product" || "$1" == "service" ) ]]; then
  mode="$1"
  shift
fi

action="run"
if [[ "$mode" == "service" ]]; then
  action="start"
fi
if [[ $# -gt 0 ]]; then
  case "$1" in
    configure|build|run|start|stop|status) action="$1"; shift ;;
  esac
fi

profile_name="generic"
instance_name=""

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
project_root="$(cd "$script_dir/.." && pwd)"
pxsys_root="$(realpath -m -- "${PXSYS_ROOT:-$project_root/deps/pxa-system}")"
if [[ -t 1 && "${PXA_SIMULATOR_COLOR:-1}" != "0" && -z "${NO_COLOR:-}" ]]; then
  log_reset=$'\033[0m'
  log_green=$'\033[1;32m'
  log_cyan=$'\033[1;36m'
  log_yellow=$'\033[1;33m'
else
  log_reset=""
  log_green=""
  log_cyan=""
  log_yellow=""
fi
log_success() { printf '%s%s%s\n' "$log_green" "$1" "$log_reset"; }
log_info() { printf '%s%s%s\n' "$log_cyan" "$1" "$log_reset"; }
log_notice() { printf '%s%s%s\n' "$log_yellow" "$1" "$log_reset"; }
app_root="${PXA_SIMULATOR_APP_SOURCE_ROOT:-$pxsys_root/apps/pxa}"
package_path=""
installed_id=""
state_root=""
publisher_key="${PXA_SIMULATOR_PUBLISHER_KEY:-}"
tcp_listen=""
pxadb_autostart_started=0
pxadb_lease_path=""
simulator_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --profile)
      [[ $# -ge 2 ]] || { echo "--profile requires a name" >&2; exit 2; }
      profile_name="$2"
      shift 2
      ;;
    --instance)
      [[ $# -ge 2 ]] || { echo "--instance requires a name" >&2; exit 2; }
      instance_name="$2"
      shift 2
      ;;
    --app-root)
      [[ $# -ge 2 ]] || { echo "--app-root requires a path" >&2; exit 2; }
      app_root="$2"
      shift 2
      ;;
    --package)
      [[ $# -ge 2 ]] || { echo "--package requires a path" >&2; exit 2; }
      package_path="$2"
      shift 2
      ;;
    --installed)
      [[ $# -ge 2 ]] || { echo "--installed requires an app ID" >&2; exit 2; }
      installed_id="$2"
      shift 2
      ;;
    --state-root)
      [[ $# -ge 2 ]] || { echo "--state-root requires a path" >&2; exit 2; }
      state_root="$2"
      shift 2
      ;;
    --publisher-key)
      [[ $# -ge 2 ]] || { echo "--publisher-key requires a path" >&2; exit 2; }
      publisher_key="$2"
      shift 2
      ;;
    --listen)
      [[ $# -ge 2 ]] || { echo "--listen requires HOST:PORT" >&2; exit 2; }
      tcp_listen="$2"
      shift 2
      ;;
    --) shift; simulator_args=("$@"); break ;;
    *) usage >&2; exit 2 ;;
  esac
done
if [[ ! "$profile_name" =~ ^[a-z0-9-]+$ ]]; then
  echo "Invalid simulator profile: $profile_name" >&2
  exit 2
fi
if [[ -n "$instance_name" && ! "$instance_name" =~ ^[a-z0-9-]+$ ]]; then
  echo "Invalid simulator instance: $instance_name" >&2
  exit 2
fi
simulator_id="$profile_name"
if [[ -n "$instance_name" ]]; then
  simulator_id+="@$instance_name"
fi
if [[ "$tcp_listen" == *":auto" ]]; then
  tcp_listen="${tcp_listen%:auto}:0"
fi
profile="$project_root/simulator/profiles/$profile_name.toml"
if [[ ! -f "$profile" ]]; then
  echo "Simulator profile is unavailable: $profile" >&2
  exit 2
fi
if [[ ! -f "$pxsys_root/simulator/desktop/CMakeLists.txt" ]]; then
  echo "Desktop simulator is unavailable: $pxsys_root/simulator/desktop" >&2
  exit 1
fi
if [[ -z "$state_root" ]]; then
  state_root="$project_root/local/simulator/$simulator_id"
fi
state_root="$(realpath -m -- "$state_root")"
socket_root="${PXA_SIMULATOR_SOCKET_ROOT:-/tmp/pxa-simulator-${UID}}"
control_socket="$socket_root/$simulator_id.control.sock"
if [[ -z "$publisher_key" ]]; then
  publisher_key="$app_root/.dev-signing/publisher-public.der"
fi
if [[ -n "$installed_id" && ! "$installed_id" =~ ^pxa-[a-z0-9._-]{1,60}$ ]]; then
  echo "Invalid installed PXA app ID: $installed_id" >&2
  exit 2
fi
if [[ -n "$package_path" && -n "$installed_id" ]]; then
  echo "--package and --installed cannot be used together" >&2
  exit 2
fi

mapfile -t profile_args < <("${PYTHON:-python3}" - "$profile" <<'PYTHON'
import sys
try:
    import tomllib
except ModuleNotFoundError as error:
    raise SystemExit("Python 3.11+ is required to read simulator profiles") from error

with open(sys.argv[1], "rb") as source:
    desktop = tomllib.load(source).get("desktop", {})
if not isinstance(desktop, dict):
    raise SystemExit("[desktop] must be a table")
for key in ("width", "height"):
    value = desktop.get(key)
    if not isinstance(value, int) or value <= 0:
        raise SystemExit(f"desktop.{key} must be a positive integer")
    print(f"--{key}")
    print(value)
corner_radius = desktop.get("corner_radius", 0)
if not isinstance(corner_radius, int) or corner_radius < 0:
    raise SystemExit("desktop.corner_radius must be a non-negative integer")
if corner_radius > min(desktop["width"], desktop["height"]) // 2:
    raise SystemExit("desktop.corner_radius exceeds half of the display's shortest side")
if corner_radius:
    print("--corner-radius")
    print(corner_radius)
safe_insets = desktop.get("safe_insets", [0, 0, 0, 0])
if (not isinstance(safe_insets, list) or len(safe_insets) != 4 or
        any(not isinstance(value, int) or value < 0 or value > 65535
            for value in safe_insets)):
    raise SystemExit("desktop.safe_insets must be [top, right, bottom, left] in 0..65535")
if any(safe_insets):
    print("--safe-insets")
    print(",".join(str(value) for value in safe_insets))
shape_background = desktop.get("shape_background", "matte")
if shape_background not in ("matte", "black"):
    raise SystemExit("desktop.shape_background must be matte or black")
print("--shape-background")
print(shape_background)
locale = desktop.get("locale", "en-US")
if not isinstance(locale, str) or not locale:
    raise SystemExit("desktop.locale must be a non-empty string")
print("--locale")
print(locale)
theme = desktop.get("theme", "dark")
if theme not in ("dark", "light", "custom"):
    raise SystemExit("desktop.theme must be dark, light or custom")
print(f"--{theme}" if theme != "custom" else "--custom-theme")
if desktop.get("round", False):
    if corner_radius:
        raise SystemExit("desktop.corner_radius and desktop.round cannot be used together")
    print("--round")
if desktop.get("gestures", False):
    print("--gestures")
PYTHON
)

build_dir="$project_root/build/simulator/$profile_name"
cmake_bin="${CMAKE:-cmake}"
product_args=("${profile_args[0]}" "${profile_args[1]}"
              "${profile_args[2]}" "${profile_args[3]}")
for ((profile_index = 0; profile_index < ${#profile_args[@]}; ++profile_index)); do
  if [[ "${profile_args[profile_index]}" == "--locale" ]]; then
    product_args+=("--locale" "${profile_args[profile_index + 1]}")
    break
  fi
done
if [[ "$mode" == "ui" ]]; then
  simulator_target="pxsys_desktop_simulator"
else
  simulator_target="pxsys_product_simulator"
fi
configure() {
  "$cmake_bin" -S "$pxsys_root/simulator/desktop" -B "$build_dir" \
    -DPXSYS_DESKTOP_APP_SOURCE_ROOT="$(realpath -m -- "$app_root")"
}

print_pxadb_endpoint() {
  local socket_root="${PXA_SIMULATOR_SOCKET_ROOT:-/tmp/pxa-simulator-${UID}}"
  local socket_path="$socket_root/$simulator_id.sock"
  local control_socket="$socket_root/$simulator_id.control.sock"
  local tcp_endpoint_path="$state_root/pxadb2.tcp"
  local tcp_endpoint=""
  [[ -S "$socket_path" ]] || return 0
  log_success "Simulator ready: profile=$profile_name${instance_name:+ instance=$instance_name} mode=$mode"
  log_info "PXADB2 local endpoint: unix:$socket_path"
  log_info "PXADB2 debug control: unix:$control_socket"
  log_info "PXADB command: pxadb --simulator $simulator_id <command>"
  if [[ -f "$tcp_endpoint_path" ]]; then
    tcp_endpoint="$(<"$tcp_endpoint_path")"
    [[ -n "$tcp_endpoint" ]] && log_info "PXADB2 TCP listener: tcp://$tcp_endpoint (token: $state_root/pxadb2.token)"
  fi
}

service() {
  local socket_root="${PXA_SIMULATOR_SOCKET_ROOT:-/tmp/pxa-simulator-${UID}}"
  local socket_path="$socket_root/$simulator_id.sock"
  local control_socket="$socket_root/$simulator_id.control.sock"
  local pid_path="$state_root/pxadb.pid"
  local log_path="$state_root/pxadb.log"
  local installer="$build_dir/pxsys_package_installer"
  local token_path="$state_root/pxadb2.token"
  local tcp_endpoint_path="$state_root/pxadb2.tcp"
  local tcp_args=()
  local tcp_endpoint=""
  local pid=""
  case "$action" in
    start)
      if [[ -f "$pid_path" ]]; then
        pid="$(<"$pid_path")"
        if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
          if [[ -n "$tcp_listen" && ! -f "$tcp_endpoint_path" ]]; then
            echo "PXADB2 service is already running without a TCP listener; stop it before using --listen" >&2
            return 1
          fi
          log_notice "PXADB2 service already running: socket=$socket_path state=$state_root log=$log_path"
          [[ -f "$tcp_endpoint_path" ]] && log_info "PXADB2 TCP listener: tcp://$(<"$tcp_endpoint_path") (token: $token_path)"
          return 0
        fi
        rm -f -- "$pid_path"
      fi
      mkdir -p -- "$socket_root"
      chmod 700 -- "$socket_root"
      rm -f -- "$socket_path" "$tcp_endpoint_path"
      mkdir -p -- "$state_root"
      configure
      "$cmake_bin" --build "$build_dir" --target pxsys_package_installer
      [[ -f "$publisher_key" ]] || { echo "Publisher key is unavailable: $publisher_key" >&2; return 2; }
      if [[ -n "$tcp_listen" ]]; then
        tcp_args=(--tcp-listen "$tcp_listen" --token-file "$token_path" --tcp-address-file "$tcp_endpoint_path")
      fi
      nohup "${PYTHON:-python3}" "$script_dir/simulator_pxadb.py" \
        --state-root "$state_root" --socket "$socket_path" \
        --installer "$installer" --publisher-key "$publisher_key" \
        --control-socket "$control_socket" "${tcp_args[@]}" \
        >"$log_path" 2>&1 < /dev/null &
      pid="$!"
      printf '%s\n' "$pid" > "$pid_path"
      for _ in {1..50}; do
        [[ -S "$socket_path" && ( -z "$tcp_listen" || -s "$tcp_endpoint_path" ) ]] && break
        if ! kill -0 "$pid" 2>/dev/null; then
          cat "$log_path" >&2 || true
          rm -f -- "$pid_path"
          return 1
        fi
        sleep 0.05
      done
      [[ -S "$socket_path" ]] || { echo "Simulator PXADB service did not create its socket" >&2; return 1; }
      [[ -z "$tcp_listen" || -s "$tcp_endpoint_path" ]] || { echo "Simulator PXADB service did not create its TCP listener" >&2; return 1; }
      log_success "PXADB2 service started: socket=$socket_path state=$state_root log=$log_path"
      pxadb_autostart_started=1
      if [[ -n "$tcp_listen" ]]; then
        tcp_endpoint="$(<"$tcp_endpoint_path")"
        log_info "PXADB2 TCP service started: $tcp_endpoint (token: $token_path)"
      fi
      ;;
    stop)
      [[ -f "$pid_path" ]] || { echo "Simulator PXADB service is not running"; return 0; }
      pid="$(<"$pid_path")"
      if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid"
        for _ in {1..50}; do
          kill -0 "$pid" 2>/dev/null || break
          sleep 0.05
        done
      fi
      rm -f -- "$pid_path" "$socket_path" "$tcp_endpoint_path" \
          "$control_socket" \
          "$state_root/pxadb.autostart"
      log_notice "Simulator PXADB service stopped"
      ;;
    status)
      if [[ -f "$pid_path" ]]; then
        pid="$(<"$pid_path")"
        if [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null && [[ -S "$socket_path" ]]; then
          echo "Simulator PXADB service is running: $socket_path"
          [[ -f "$tcp_endpoint_path" ]] && echo "Simulator PXADB TCP listener: $(<"$tcp_endpoint_path")"
          return 0
        fi
      fi
      echo "Simulator PXADB service is not running"
      return 1
      ;;
    *)
      echo "service mode supports start, stop or status" >&2
      return 2
      ;;
  esac
}

if [[ "$mode" == "service" ]]; then
  service
  service_status=$?
  if [[ "$action" == "start" && "$service_status" == "0" ]]; then
    rm -f -- "$state_root/pxadb.autostart"
  fi
  exit "$service_status"
fi

autostart_pxadb() {
  local saved_action="$action" marker="$state_root/pxadb.autostart"
  local lease_dir="$state_root/pxadb.leases" lease="" lease_pid=""
  if [[ "${PXA_SIMULATOR_AUTOSTART_PXADB:-1}" == "0" ]]; then
    echo "PXADB2 service autostart disabled"
    return 0
  fi
  action="start"
  service
  action="$saved_action"
  if [[ "$pxadb_autostart_started" == "1" ]]; then
    printf '%s\n' "auto" > "$marker"
  fi
  [[ -f "$marker" ]] || return 0
  mkdir -p -- "$lease_dir"
  for lease in "$lease_dir"/*; do
    [[ -e "$lease" ]] || continue
    lease_pid="${lease##*/}"
    if [[ ! "$lease_pid" =~ ^[0-9]+$ ]] || ! kill -0 "$lease_pid" 2>/dev/null; then
      rm -f -- "$lease"
    fi
  done
  pxadb_lease_path="$lease_dir/$$"
  printf '%s\n' "$(date +%s)" > "$pxadb_lease_path"
}

stop_owned_pxadb() {
  local status=$? marker="$state_root/pxadb.autostart"
  local lease_dir="$state_root/pxadb.leases" lease="" lease_pid="" active=0
  trap - EXIT
  if [[ -n "$pxadb_lease_path" ]]; then
    rm -f -- "$pxadb_lease_path"
    pxadb_lease_path=""
  fi
  if [[ -f "$marker" && -d "$lease_dir" ]]; then
    for lease in "$lease_dir"/*; do
      [[ -e "$lease" ]] || continue
      lease_pid="${lease##*/}"
      if [[ "$lease_pid" =~ ^[0-9]+$ ]] && kill -0 "$lease_pid" 2>/dev/null; then
        active=1
      else
        rm -f -- "$lease"
      fi
    done
  fi
  if [[ -f "$marker" && "$active" == "0" ]]; then
    action="stop"
    service || true
  fi
  return "$status"
}

case "$action" in
  configure) exec "$cmake_bin" -S "$pxsys_root/simulator/desktop" -B "$build_dir" \
      -DPXSYS_DESKTOP_APP_SOURCE_ROOT="$(realpath -m -- "$app_root")" ;;
  build) configure; exec "$cmake_bin" --build "$build_dir" ;;
  run)
      autostart_pxadb
      trap stop_owned_pxadb EXIT
      configure
      if [[ "$mode" == "ui" ]]; then
          "$cmake_bin" --build "$build_dir" --target pxsys_desktop_simulator pxsys_product_simulator
      else
          "$cmake_bin" --build "$build_dir" --target "$simulator_target"
      fi
      print_pxadb_endpoint
      if [[ "$mode" == "ui" ]]; then
          [[ -f "$publisher_key" ]] || { echo "Publisher key is unavailable: $publisher_key" >&2; exit 2; }
          "$build_dir/pxsys_desktop_simulator" "${profile_args[@]}" \
              --installed-packages-root "$state_root/packages" \
              --product-runner "$build_dir/pxsys_product_simulator" \
              --publisher-key "$(realpath -m -- "$publisher_key")" \
              --state-root "$state_root" \
              --pxadb-control-socket "$control_socket" \
              "${simulator_args[@]}"
          exit $?
      fi
      if [[ -n "$installed_id" ]]; then
          package_path="$state_root/packages/$installed_id"
      fi
      [[ -n "$package_path" ]] || { echo "product mode requires --package or --installed" >&2; exit 2; }
      [[ -d "$package_path" ]] || { echo "PXA package directory is unavailable: $package_path" >&2; exit 2; }
      [[ -f "$publisher_key" ]] || { echo "Publisher key is unavailable: $publisher_key" >&2; exit 2; }
      "$build_dir/pxsys_product_simulator" --package "$(realpath -m -- "$package_path")" \
          --publisher-key "$(realpath -m -- "$publisher_key")" \
          --state-root "$state_root" \
          --pxadb-control-socket "$control_socket" \
          "${product_args[@]}" "${simulator_args[@]}"
      exit $?
      ;;
  *) usage >&2; exit 2 ;;
esac
