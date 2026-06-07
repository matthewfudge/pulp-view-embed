#!/usr/bin/env bash
# package-sdk.sh — package an installed Pulp SDK into a relocatable tarball.
#
# Consumers of pulp-view-embed need a Pulp SDK that find_package(Pulp CONFIG)
# can locate, but building one from the embedding-seam branch is slow and
# requires the full Pulp toolchain. This script snapshots an already-installed
# SDK (the output of `cmake --install <pulp-build> --prefix <sdk>`) into a
# single relocatable tarball: a consumer untars it and points
# CMAKE_PREFIX_PATH at the result — no Pulp build, no branch checkout.
#
# The installed SDK is already relocatable: PulpConfig.cmake resolves paths via
# CMAKE_CURRENT_LIST_DIR / PACKAGE_PREFIX_DIR, every shipped dylib uses an
# @rpath install_name, and no cmake config bakes a build-machine path. This
# script therefore just verifies those invariants and tars the tree.
#
# Usage:
#   tools/package-sdk.sh [--sdk DIR] [--out FILE.tar.gz] [--mac-only] [--no-verify]
#
#   --sdk DIR        Installed SDK prefix to package.
#                    Default: $PULP_SDK_INSTALL or /Volumes/Workshop/Code/pulp-sdk-install
#   --out FILE       Output tarball path.
#                    Default: ./dist/pulp-sdk-<version>-<os>-<arch>.tar.gz
#   --mac-only       Drop the iOS/visionOS Skia slices (external/skia-build/build/ios-*),
#                    roughly halving the tarball for a macOS-only consumer.
#   --no-verify      Skip the relocatability invariant checks (faster).
set -euo pipefail

SDK_DIR="${PULP_SDK_INSTALL:-/Volumes/Workshop/Code/pulp-sdk-install}"
OUT=""
MAC_ONLY=0
VERIFY=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sdk) SDK_DIR="$2"; shift 2;;
        --out) OUT="$2"; shift 2;;
        --mac-only) MAC_ONLY=1; shift;;
        --no-verify) VERIFY=0; shift;;
        -h|--help) sed -n '2,40p' "$0"; exit 0;;
        *) echo "unknown arg: $1" >&2; exit 2;;
    esac
done

if [[ ! -d "$SDK_DIR" ]]; then
    echo "error: SDK dir not found: $SDK_DIR" >&2
    exit 1
fi
if [[ ! -f "$SDK_DIR/lib/cmake/Pulp/PulpConfig.cmake" ]]; then
    echo "error: $SDK_DIR does not look like an installed Pulp SDK (no PulpConfig.cmake)" >&2
    exit 1
fi

VERSION="$(tr -d '[:space:]' < "$SDK_DIR/version.txt" 2>/dev/null || echo unknown)"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"

if [[ -z "$OUT" ]]; then
    mkdir -p dist
    OUT="dist/pulp-sdk-${VERSION}-${OS}-${ARCH}.tar.gz"
fi

# ── Relocatability invariants ────────────────────────────────────────────────
if [[ "$VERIFY" == "1" ]]; then
    echo "verifying SDK relocatability…"
    # No build-machine absolute paths in the cmake configs.
    if grep -rl '/Volumes/Workshop\|/Users/' "$SDK_DIR/lib/cmake" >/dev/null 2>&1; then
        echo "error: absolute build-machine paths found in $SDK_DIR/lib/cmake" >&2
        grep -rl '/Volumes/Workshop\|/Users/' "$SDK_DIR/lib/cmake" >&2
        exit 1
    fi
    # Every shipped dylib must use an @rpath install_name (relocatable).
    if command -v otool >/dev/null 2>&1; then
        while IFS= read -r dylib; do
            name="$(otool -D "$dylib" | tail -1)"
            case "$name" in
                @rpath/*|@loader_path/*) ;;
                *) echo "error: $dylib has non-relocatable install_name: $name" >&2; exit 1;;
            esac
        done < <(find "$SDK_DIR" -name '*.dylib')
    fi
    echo "  ok — no absolute paths, all dylibs @rpath-relative"
fi

# ── Stage + tar ──────────────────────────────────────────────────────────────
# Stage so we can optionally prune iOS slices without mutating the source SDK,
# and so the tarball unpacks to a single top-level dir.
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
TOP="pulp-sdk-${VERSION}"
echo "staging $SDK_DIR -> $STAGE/$TOP …"
# -a preserves symlinks/perms; the SDK dylibs and skia .a files matter.
cp -a "$SDK_DIR" "$STAGE/$TOP"

if [[ "$MAC_ONLY" == "1" ]]; then
    echo "pruning non-macOS Skia slices (--mac-only)…"
    rm -rf "$STAGE/$TOP/external/skia-build/build/ios-gpu" \
           "$STAGE/$TOP/external/skia-build/build/visionos-gpu" 2>/dev/null || true
fi

mkdir -p "$(dirname "$OUT")"
echo "writing $OUT …"
# Use COPYFILE_DISABLE so macOS tar does not embed ._ AppleDouble files.
COPYFILE_DISABLE=1 tar -C "$STAGE" -czf "$OUT" "$TOP"

SIZE="$(du -h "$OUT" | cut -f1)"
echo "done: $OUT ($SIZE)"
echo
echo "Consumer usage:"
echo "  tar xzf $(basename "$OUT")"
echo "  cmake -S . -B build -DCMAKE_PREFIX_PATH=\$PWD/$TOP -DPULP_VIEW_EMBED_SHARED=ON"
