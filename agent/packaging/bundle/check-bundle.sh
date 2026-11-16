#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
# SPDX-License-Identifier: GPL-3.0-or-later

# Checks a kemu-agent bundle tree against the glibc it promises to run on:
#
#   check-bundle.sh [--max-glibc 2.28] <bundle-root>
#
# For every ELF file below <bundle-root>:
#   - the newest GLIBC_x.y symbol version it imports is not above --max-glibc;
#   - it needs libstdc++ and libgcc_s from nowhere (they are linked statically);
#   - every other library it needs is either part of glibc or in the bundle.
# The executable (usr/bin/kemu-agent) must also have an $ORIGIN run path.
# Needs objdump (binutils). Exit status 1 when anything fails.

set -u

max=2.28
while [ $# -gt 0 ]; do
    case "$1" in
    --max-glibc) max="${2:?--max-glibc needs a version}"; shift 2 ;;
    -h | --help) sed -n '5,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    --) shift; break ;;
    -*) echo "check-bundle: unknown option $1" >&2; exit 2 ;;
    *) break ;;
    esac
done
root="${1:?usage: check-bundle.sh [--max-glibc VERSION] BUNDLE-ROOT}"
[ -d "$root" ] || { echo "check-bundle: $root is not a directory" >&2; exit 2; }
command -v objdump > /dev/null || { echo "check-bundle: objdump (binutils) is needed" >&2; exit 2; }

# What glibc itself provides; everything else must travel with the bundle.
glibc_libs='^(libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|libresolv\.so\.2|libutil\.so\.1|libnsl\.so\.1|libanl\.so\.1|ld-linux-[a-z0-9_-]+\.so\.[0-9]+|ld64\.so\.[0-9]+|linux-vdso\.so\.1)$'

is_elf() { [ "$(head -c4 "$1" 2> /dev/null | od -An -c | tr -d ' ')" = '177ELF' ]; }
newest() { sort -Vu | tail -1; }

fail=0
checked=0
while IFS= read -r -d '' f; do
    is_elf "$f" || continue
    checked=$((checked + 1))
    rel="${f#"$root"/}"
    problems=()

    glibc="$(objdump -T "$f" 2> /dev/null | grep -F '*UND*' | grep -o 'GLIBC_[0-9][0-9.]*' | sed 's/^GLIBC_//' | newest)"
    if [ -n "$glibc" ] && [ "$(printf '%s\n%s\n' "$glibc" "$max" | newest)" != "$max" ]; then
        problems+=("needs glibc $glibc (limit $max)")
    fi

    while IFS= read -r lib; do
        [ -n "$lib" ] || continue
        case "$lib" in
        libstdc++.so* | libgcc_s.so*)
            problems+=("needs $lib: link libstdc++ and libgcc statically")
            ;;
        *)
            if ! [[ "$lib" =~ $glibc_libs ]] && [ -z "$(find "$root" -name "$lib" -print -quit)" ]; then
                problems+=("needs $lib, which is neither glibc nor in the bundle")
            fi
            ;;
        esac
    done < <(objdump -p "$f" 2> /dev/null | awk '$1 == "NEEDED" { print $2 }')

    if [ "$rel" = usr/bin/kemu-agent ] && ! objdump -p "$f" 2> /dev/null | grep -E '(RUNPATH|RPATH)' | grep -qF '$ORIGIN'; then
        problems+=("has no \$ORIGIN run path")
    fi

    if [ ${#problems[@]} -eq 0 ]; then
        printf 'ok    %-52s glibc %s\n' "$rel" "${glibc:--}"
    else
        fail=1
        for p in "${problems[@]}"; do printf 'FAIL  %-52s %s\n' "$rel" "$p"; done
    fi
done < <(find "$root" -type f -print0 | sort -z)

if [ "$checked" -eq 0 ]; then
    echo "check-bundle: no ELF files found below $root" >&2
    exit 1
fi
if [ "$fail" -ne 0 ]; then
    echo "check-bundle: FAILED (glibc limit $max)" >&2
    exit 1
fi
echo "check-bundle: $checked ELF files fine (glibc limit $max)"
