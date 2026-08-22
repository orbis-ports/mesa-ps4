#!/usr/bin/env bash
# Cross-check the Tempest fork's CIK tiler against AMD's addrlib, on the host.
#
#   tools/tilecheck.sh [--tempest <path>] [-v]
#
# Needs a completed host build for addrlib's objects - ./build.sh --host-orbis leaves them in
# build-hostorbis. Nothing about the console is involved; this is two address functions and a comparison.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEMPEST="${HOME}/src/Tempest"
WORK="${HOME}/.cache/orbis-mesa"
VERBOSE=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --tempest) TEMPEST="$2"; shift 2 ;;
    -v) VERBOSE=(-v); shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

MESA="$(git -C "${ROOT}" rev-parse --show-toplevel)"
BUILD="${MESA}/build-hostorbis"

[[ -f "${TEMPEST}/Engine/gapi/gnm/gnmtiler.cpp" ]] || {
  echo "tilecheck: no gnmtiler.cpp under ${TEMPEST} (--tempest <path>)" >&2; exit 1; }
[[ -d "${BUILD}" ]] || {
  echo "tilecheck: no host build at ${BUILD} - run ./build.sh --host-orbis first" >&2; exit 1; }

# addrlib is a static library in the host build. Link it whole: the entry points are reached by name from
# our own code rather than from anything meson recorded.
ADDRLIB="$(find "${BUILD}" -name 'libaddrlib.a' -o -name 'libamdgpu_addrlib.a' | head -1)"
[[ -n "${ADDRLIB}" ]] || {
  echo "tilecheck: no addrlib archive under ${BUILD}" >&2; exit 1; }

echo "tilecheck: addrlib  ${ADDRLIB}"
echo "tilecheck: tiler    ${TEMPEST}/Engine/gapi/gnm/gnmtiler.cpp"

OUT="${WORK}/tilecheck"
nix develop nixpkgs#mesa --command c++ -std=c++17 -O1 -g -o "${OUT}" \
  "${ROOT}/tools/tilecheck.cpp" \
  "${TEMPEST}/Engine/gapi/gnm/gnmtiler.cpp" \
  -I"${TEMPEST}/Engine/gapi/gnm" \
  -I"${MESA}/src/amd/common" \
  -I"${MESA}/src/amd/addrlib/inc" \
  -I"${MESA}/src/amd/addrlib/src" \
  -I"${MESA}/src/amd/addrlib/src/core" \
  -I"${MESA}/src/amd/addrlib" \
  "${ADDRLIB}" -lstdc++ -lm

echo
"${OUT}" "${VERBOSE[@]}"
