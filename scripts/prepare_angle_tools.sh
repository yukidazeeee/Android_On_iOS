#!/bin/bash
# Bootstrap both depot_tools checkouts before ANGLE's dependency hooks run.
set -euo pipefail
angle_dir="${1:?Pass the ANGLE checkout directory}"
outer_tools="${2:?Pass the pinned external depot_tools directory}"
angle_dir="$(cd "$angle_dir" && pwd)"
outer_tools="$(cd "$outer_tools" && pwd)"
export DEPOT_TOOLS_UPDATE=0
export DEPOT_TOOLS_BOOTSTRAP_PYTHON3=1

bootstrap_tools() {
  local tools_dir="$1"
  # ensure_bootstrap honors DEPOT_TOOLS_DIR, so an inherited outer path must
  # never cause the nested checkout to bootstrap the wrong directory.
  DEPOT_TOOLS_DIR="$tools_dir" "$tools_dir/ensure_bootstrap"
  if [[ ! -s "$tools_dir/python3_bin_reldir.txt" ]]; then
    echo "depot_tools Python bootstrap failed: $tools_dir/python3_bin_reldir.txt missing or empty" >&2
    return 1
  fi
  DEPOT_TOOLS_DIR="$tools_dir" "$tools_dir/python3" --version
}

bootstrap_tools "$outer_tools"
export PATH="$outer_tools:$PATH"
cd "$angle_dir"
# configure_siso.py invokes the DEPS checkout's python3 wrapper. That checkout
# only exists after sync; running hooks during sync would bootstrap too late.
DEPOT_TOOLS_DIR="$outer_tools" "$outer_tools/gclient" sync --no-history --shallow --nohooks
bundled_tools="$angle_dir/third_party/depot_tools"
bootstrap_tools "$bundled_tools"
export PATH="$bundled_tools:$PATH"
DEPOT_TOOLS_DIR="$bundled_tools" "$bundled_tools/gclient" runhooks
