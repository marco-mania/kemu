#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
# SPDX-License-Identifier: GPL-3.0-or-later

# Runs inside the toolchain container (see Containerfile), started by
# build-bundle.sh: builds kemu-agent against the static Qt in /opt/qt, puts the
# libraries that do not belong to glibc beside it, checks the result and packs
# it as /out/kemu-agent-<version>-linux-<arch>.tar.gz.
#
# Expects the agent source mounted read-only at /src and these variables:
#   GLIBC_MAX          the glibc the bundle promises to run on (e.g. 2.28)
#   QT_VERSION         the Qt that /opt/qt holds (for the bundle's info file)
#   SOURCE_DATE_EPOCH  timestamp for the archive
#   HOST_UID, HOST_GID owner of the files in /out

set -euo pipefail

: "${GLIBC_MAX:?}" "${QT_VERSION:?}" "${SOURCE_DATE_EPOCH:?}"
arch="$(uname -m)"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
work=/tmp/work
rm -rf "$work"
mkdir -p "$work" /out

echo "== configure"
cmake -S /src -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_PREFIX_PATH="/opt/qt;/opt/deps" \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    -DBUILD_TESTING=OFF

echo "== build"
cmake --build "$work/build" --parallel "$(nproc)"

version="$(sed -n 's/^CMAKE_PROJECT_VERSION:[A-Z]*=//p' "$work/build/CMakeCache.txt")"
name="kemu-agent-${version}-linux-${arch}"
root="$work/$name"
DESTDIR="$root" cmake --install "$work/build"

exe="$root/usr/bin/kemu-agent"
libdir="$root/usr/lib/kemu-agent"
mkdir -p "$libdir"

echo "== bundle libraries"
# Everything the executable needs, except what glibc provides. The checker
# below holds the same list of exceptions and fails if one is missed.
glibc_libs='^(libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|libresolv\.so\.2|libutil\.so\.1|libnsl\.so\.1|libanl\.so\.1|ld-linux-[a-z0-9_-]+\.so\.[0-9]+|ld64\.so\.[0-9]+|linux-vdso\.so\.1)$'
LD_LIBRARY_PATH="/opt/deps/lib:/opt/deps/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    ldd "$exe" | awk '$2 == "=>" && $3 ~ /^\// { print $3 }' | while read -r lib; do
    base="$(basename "$lib")"
    if ! [[ "$base" =~ $glibc_libs ]]; then
        cp -L "$lib" "$libdir/$base"
        echo "  $base"
    fi
done

strip --strip-unneeded "$exe"
find "$libdir" -type f -name '*.so*' -exec strip --strip-unneeded {} +
patchelf --set-rpath '$ORIGIN/../lib/kemu-agent' "$exe"
find "$libdir" -type f -name '*.so*' -exec patchelf --set-rpath '$ORIGIN' {} +

echo "== check"
"$here/check-bundle.sh" --max-glibc "$GLIBC_MAX" "$root"
"$exe" --version

echo "== pack"
cp "$here/install.sh" "$here/uninstall.sh" "$root/"
chmod 0755 "$root/install.sh" "$root/uninstall.sh"
cat > "$root/BUNDLE-INFO" << INFO
kemu-agent ${version}
architecture ${arch}
glibc ${GLIBC_MAX} or newer
qt ${QT_VERSION} (static)
INFO
(cd "$root" && find . -type f -o -type l | sed 's|^\./||' | sort > MANIFEST \
    && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS)
tar --sort=name --mtime="@${SOURCE_DATE_EPOCH}" --owner=0 --group=0 --numeric-owner \
    -C "$work" -czf "/out/${name}.tar.gz" "$name"
(cd /out && sha256sum "${name}.tar.gz" > "${name}.tar.gz.sha256")
chown "${HOST_UID:-0}:${HOST_GID:-0}" "/out/${name}.tar.gz" "/out/${name}.tar.gz.sha256"
echo "== /out/${name}.tar.gz"
