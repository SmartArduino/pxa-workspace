#!/usr/bin/env bash
set -euo pipefail

# Launch the PXADB GUI against the workspace client without an install.
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export PYTHONPATH="$script_dir/pxadb-gui:$script_dir/pxadb${PYTHONPATH:+:$PYTHONPATH}"
exec "${PYTHON:-python3}" -m pxadb_gui "$@"
