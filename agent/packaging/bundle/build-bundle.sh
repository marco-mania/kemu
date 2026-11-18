#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Marco Nelles
# SPDX-License-Identifier: GPL-3.0-or-later

# Builds the portable kemu-agent bundle in a container:
#
#   build-bundle.sh [--arch x86_64|aarch64] [--glibc 2.28] [--out DIR]
#                   [--engine podman|docker] [--qt-version X.Y.Z]
#
# The container is based on the manylinux image for the chosen glibc, so the
# binary needs nothing newer; Qt is built static inside it (once: the toolchain
# image is cached). The result is DIR/kemu-agent-<version>-linux-<arch>.tar.gz
# with a .sha256 beside it. See README.md ("Portable bundle").
#
# aarch64 on an x86_64 machine runs under QEMU user-mode emulation (binfmt);
# the first build of Qt then takes a long time.

set -euo pipefail

arch="$(uname -m)"
glibc=2.28
qt_version=6.11.2
out="$PWD/dist"
engine=

while [ $# -gt 0 ]; do
    case "$1" in
    --arch) arch="${2:?}"; shift 2 ;;
    --glibc) glibc="${2:?}"; shift 2 ;;
    --out) out="${2:?}"; shift 2 ;;
    --engine) engine="${2:?}"; shift 2 ;;
    --qt-version) qt_version="${2:?}"; shift 2 ;;
    -h | --help) sed -n '5,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build-bundle: unknown option $1" >&2; exit 2 ;;
    esac
done

case "$arch" in
x86_64 | aarch64) ;;
amd64) arch=x86_64 ;;
arm64) arch=aarch64 ;;
*) echo "build-bundle: unsupported architecture $arch (x86_64 or aarch64)" >&2; exit 2 ;;
esac
case "$glibc" in
2.28 | 2.34) ;;
*) echo "build-bundle: no manylinux image for glibc $glibc (2.28 or 2.34)" >&2; exit 2 ;;
esac

# Through sudo the toolchain image would go to root's container storage and
# the build directory would get files owned by root. (Plain root, as in CI
# containers, is fine.)
if [ "$(id -u)" = 0 ] && [ -n "${SUDO_USER:-}" ]; then
    echo "build-bundle: do not build the bundle with sudo. Build as your user (cmake --build <build>)," >&2
    echo "then install as root (sudo cmake --install <build>), which does not build anything." >&2
    exit 1
fi

if [ -z "$engine" ]; then
    for e in podman docker; do
        command -v "$e" > /dev/null && { engine="$e"; break; }
    done
fi
[ -n "$engine" ] || { echo "build-bundle: neither podman nor docker found" >&2; exit 1; }

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
agent="$(cd "$here/../.." && pwd)"
[ -f "$agent/CMakeLists.txt" ] || { echo "build-bundle: $agent is not the agent directory" >&2; exit 1; }

platform=
[ "$arch" = "$(uname -m)" ] || {
    case "$arch" in x86_64) platform=linux/amd64 ;; aarch64) platform=linux/arm64 ;; esac
}
base="quay.io/pypa/manylinux_${glibc//./_}_${arch}"
image="kemu-agent-bundle-toolchain:${arch}-glibc${glibc}-qt${qt_version}"
epoch="$(git -C "$agent" log -1 --format=%ct 2> /dev/null || date +%s)"

mkdir -p "$out"
out="$(cd "$out" && pwd)"

echo "== toolchain image $image"
"$engine" build ${platform:+--platform "$platform"} \
    --build-arg "BASE=$base" --build-arg "QT_VERSION=$qt_version" \
    -t "$image" -f "$here/Containerfile" "$here"

echo "== bundle"
# :z relabels the mounts for SELinux hosts; docker ignores nothing it needs.
"$engine" run --rm ${platform:+--platform "$platform"} \
    -v "$agent:/src:ro,z" -v "$out:/out:z" \
    -e "GLIBC_MAX=$glibc" -e "QT_VERSION=$qt_version" -e "SOURCE_DATE_EPOCH=$epoch" \
    -e "HOST_UID=$(id -u)" -e "HOST_GID=$(id -g)" \
    "$image" /src/packaging/bundle/bundle-inside.sh
