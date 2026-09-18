#!/usr/bin/env bash
# Build RADV for the PlayStation 4. Every repository in orbis-ports starts the same way:
#
#   ps4/build.sh [--host-too] [--host-orbis] [--work <dir>] [--sdk <dir>]
#
# ⚠ THE WORK IS IN build-support/orbis/build.sh AND STAYS THERE. This file is a doorway, not a
# move: build-support/ is where Mesa's own tree puts this kind of thing, and a checkout that
# reshuffles upstream's layout is a checkout that fights every rebase. What this adds is the one
# thing the other repositories in the organisation have and this one did not - the same entry
# point, in the same place, resolving the overlay the same way.
#
# SPDX-License-Identifier: MIT
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
TREE="$(cd "${HERE}/.." && pwd -P)"

# ⚠ TWO REPOSITORIES SINCE 2026-09-18, AND THE PROBE HAD TO CHANGE WITH THEM. orbis-compat is
# include/ and the archive; orbis-porting-kit is the toolchain file, the loader shim and
# scripts/ps4/. Looking for scripts/ps4/orbis-env.sh to identify the OVERLAY now finds either
# nothing or the kit - this build failed with exactly that, "No such file or directory", on the
# first run after the move. The overlay is identified by a header it owns instead.
for c in "${ORBIS_COMPAT_DIR:-}" "${TREE}/../orbis-compat" "${HOME}/src-ps4/orbis-compat"; do
  [[ -n "$c" && -f "$c/include/orbis_prefix.h" ]] && { ORBIS_COMPAT_DIR="$c"; break; }
done
[[ -n "${ORBIS_COMPAT_DIR:-}" ]] || { echo "!! orbis-compat not found - clone https://github.com/orbis-ports/orbis-compat next to this repository, or set ORBIS_COMPAT_DIR" >&2; exit 1; }
export ORBIS_COMPAT_DIR

# The kit, with the overlay last: it carried these scripts until the move, so a pinned checkout
# older than that still works.
for k in "${ORBIS_KIT_DIR:-}" "${TREE}/../orbis-porting-kit" "${HOME}/src-ps4/orbis-porting-kit" "${ORBIS_COMPAT_DIR}"; do
  [[ -n "$k" && -f "$k/scripts/ps4/orbis-env.sh" ]] && { ORBIS_KIT_DIR="$k"; break; }
done
[[ -n "${ORBIS_KIT_DIR:-}" ]] || { echo "!! orbis-porting-kit not found - clone https://github.com/orbis-ports/orbis-porting-kit next to this repository, or set ORBIS_KIT_DIR" >&2; exit 1; }
export ORBIS_KIT_DIR
. "${ORBIS_KIT_DIR}/scripts/ps4/orbis-env.sh"

exec "${TREE}/build-support/orbis/build.sh" "$@"
