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

# ⚠ The six lines that cannot be shared - see orbis-compat/scripts/ps4/orbis-env.sh.
for c in "${ORBIS_COMPAT_DIR:-}" "${TREE}/../orbis-compat" "${HOME}/src-ps4/orbis-compat"; do
  [[ -n "$c" && -f "$c/scripts/ps4/orbis-env.sh" ]] && { ORBIS_COMPAT_DIR="$c"; break; }
done
[[ -n "${ORBIS_COMPAT_DIR:-}" ]] || { echo "!! orbis-compat not found - clone https://github.com/orbis-ports/orbis-compat next to this repository, or set ORBIS_COMPAT_DIR" >&2; exit 1; }
. "${ORBIS_COMPAT_DIR}/scripts/ps4/orbis-env.sh"

exec "${TREE}/build-support/orbis/build.sh" "$@"
