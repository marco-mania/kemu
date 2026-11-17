#!/bin/sh
# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
# SPDX-License-Identifier: GPL-3.0-or-later

# Installs kemu-agent from this bundle:
#
#   sudo sh install.sh [--prefix DIR] [--no-start] [--force] [--dry-run]
#
# The program goes to DIR (default /opt/kemu-agent), the systemd user unit, the
# udev rule and the autostart entry go to /etc, which stays writable on
# image-based systems where /usr is not. Nothing is put under /usr, which
# belongs to the distribution's packages.
#
# Before installing, the script checks the bundle's checksums, the
# architecture and the glibc version. Afterwards it reloads the udev rules and
# starts the agent in the graphical sessions that are running (--no-start
# leaves them alone). Running it with a newer bundle upgrades the installation
# in place; the same version is left as it is (--reinstall installs it again),
# an older one is refused (--allow-downgrade). DIR/uninstall.sh removes
# everything it installed.
#
# Options:
#   --prefix DIR   where the program goes (default /opt/kemu-agent)
#   --no-start     do not start or restart the agent in running sessions
#   --force        install even though kemu-agent is installed otherwise
#   --reinstall    install even though this version is installed already
#   --allow-downgrade  install even though a newer version is installed
#   --dry-run      say what would be done, change nothing
#   --root DIR     install into the system tree below DIR (for images and
#                  tests); nothing is reloaded or started then
#
# Exit status: 0 done (also when this version was installed already), 1 failed,
# 2 wrong usage, 3 this system cannot run the bundle, 4 kemu-agent is installed
# otherwise (see --force), 5 a newer version is installed (see
# --allow-downgrade).

set -eu

prefix=/opt/kemu-agent
root=
start=1
force=0
reinstall=0
downgrade=0
dry=0

port=/dev/virtio-ports/org.kde.kemu.agent.0
unit_dir=/etc/systemd/user
wants_dir=/etc/systemd/user/graphical-session.target.wants
rules_dir=/etc/udev/rules.d
autostart_dir=/etc/xdg/autostart

here=$(cd "$(dirname "$0")" && pwd)

die() { # message [status]
    echo "install.sh: $1" >&2
    exit "${2:-1}"
}

usage() {
    sed -n '5,35p' "$0" | sed 's/^# \{0,1\}//'
}

# version_ge A B: true when version A >= version B (numeric, dot-separated).
version_ge() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        na = split(a, x, "."); nb = split(b, y, ".")
        n = na > nb ? na : nb
        for (i = 1; i <= n; i++) {
            if ((x[i] + 0) > (y[i] + 0)) exit 0
            if ((x[i] + 0) < (y[i] + 0)) exit 1
        }
        exit 0
    }'
}

# version_cmp A B: prints -1, 0 or 1 as version A is older than, the same as or
# newer than B. Versions are dot-separated numbers with an optional suffix
# after "-" (0.4.0-rc1), which makes a version older than the same one without
# (0.4.0); two suffixes compare as strings.
version_cmp() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        pa = index(a, "-"); pb = index(b, "-")
        ma = pa ? substr(a, 1, pa - 1) : a; sa = pa ? substr(a, pa + 1) : ""
        mb = pb ? substr(b, 1, pb - 1) : b; sb = pb ? substr(b, pb + 1) : ""
        na = split(ma, x, "."); nb = split(mb, y, ".")
        n = na > nb ? na : nb
        for (i = 1; i <= n; i++) {
            if ((x[i] + 0) > (y[i] + 0)) { print 1; exit }
            if ((x[i] + 0) < (y[i] + 0)) { print -1; exit }
        }
        if (sa == sb) print 0
        else if (sa == "") print 1
        else if (sb == "") print -1
        else print (sa > sb) ? 1 : -1
    }'
}

# --- recording what is installed -------------------------------------------

list=
record() { # type path
    list="$list$1 $2
"
}

# mkdir_p DIR: creates DIR and its missing parents, recording each one made.
mkdir_p() {
    case "$1" in /) return 0 ;; esac
    [ -d "$root$1" ] && return 0
    case "$list" in *"d $1
"*) return 0 ;; esac
    mkdir_p "$(dirname "$1")"
    if [ "$dry" = 1 ]; then
        echo "  mkdir $1"
    else
        mkdir "$root$1"
    fi
    record d "$1"
}

# put SOURCE DEST MODE: copies a file, replacing DEST in one rename so that a
# running agent keeps its executable.
put() {
    mkdir_p "$(dirname "$2")"
    if [ "$dry" = 1 ]; then
        echo "  install $2"
    else
        cp "$1" "$root$2.new.$$"
        chmod "$3" "$root$2.new.$$"
        mv -f "$root$2.new.$$" "$root$2"
    fi
    record f "$2"
}

# put_rewritten SOURCE DEST: like put, with the Exec lines pointing to the
# installed program and a marker saying which installation it belongs to.
put_rewritten() {
    tmp="${TMPDIR:-/tmp}/kemu-agent-install.$$"
    {
        echo "# Installed by kemu-agent's install.sh, prefix $prefix"
        sed -e "s|^ExecStart=.*|ExecStart=$prefix/bin/kemu-agent|" \
            -e "s|^Exec=.*|Exec=$prefix/bin/kemu-agent|" "$1"
    } > "$tmp"
    put "$tmp" "$2" 0644
    rm -f "$tmp"
}

put_link() { # target link
    mkdir_p "$(dirname "$2")"
    if [ "$dry" = 1 ]; then
        echo "  link $2 -> $1"
    else
        ln -sfn "$1" "$root$2"
    fi
    record l "$2"
}

# --- the running system ----------------------------------------------------

# for_each_session_user COMMAND...: runs "systemctl --user COMMAND..." as each
# user with a running user manager.
for_each_session_user() {
    [ -d /run/systemd/system ] || return 0
    for dir in /run/user/[0-9]*; do
        [ -S "$dir/bus" ] || continue
        uid=${dir##*/}
        [ "$uid" -ge 1000 ] 2> /dev/null || continue
        name=$(id -nu "$uid" 2> /dev/null) || continue
        runuser -u "$name" -- env XDG_RUNTIME_DIR="$dir" \
            DBUS_SESSION_BUS_ADDRESS="unix:path=$dir/bus" \
            sh -c "$1" kemu-agent-install "$name" || true
    done
}

activate() {
    if command -v restorecon > /dev/null 2>&1; then
        restorecon -R "$prefix" "$unit_dir/kemu-agent.service" \
            "$rules_dir/70-kemu-agent.rules" \
            "$autostart_dir/org.kde.kemu.agent.desktop" 2> /dev/null || true
    fi

    if command -v udevadm > /dev/null 2>&1 && [ -d /run/udev ]; then
        udevadm control --reload || true
        udevadm trigger --action=change --subsystem-match=virtio-ports || true
    fi

    if [ ! -d /run/systemd/system ]; then
        echo "No systemd here: the agent starts with the next login (autostart entry)."
        return 0
    fi
    if [ "$start" = 1 ]; then
        # $1 is expanded by the user's shell, not here.
        # shellcheck disable=SC2016
        for_each_session_user '
            systemctl --user daemon-reload
            if systemctl --user -q is-active graphical-session.target; then
                if systemctl --user restart kemu-agent.service; then
                    echo "Started kemu-agent for $1."
                else
                    echo "Could not start kemu-agent for $1; see: journalctl --user -u kemu-agent" >&2
                fi
            else
                echo "$1 has no graphical session running; kemu-agent starts at the next login."
            fi'
    else
        for_each_session_user 'systemctl --user daemon-reload'
        echo "Not started (--no-start): the agent starts with the next login."
    fi
}

# --- main ------------------------------------------------------------------

main() {
    while [ $# -gt 0 ]; do
        case "$1" in
        --prefix) [ $# -ge 2 ] || die "--prefix needs a directory" 2; prefix=$2; shift 2 ;;
        --prefix=*) prefix=${1#*=}; shift ;;
        --root) [ $# -ge 2 ] || die "--root needs a directory" 2; root=$2; shift 2 ;;
        --root=*) root=${1#*=}; shift ;;
        --no-start) start=0; shift ;;
        --force) force=1; shift ;;
        --reinstall) reinstall=1; shift ;;
        --allow-downgrade) downgrade=1; shift ;;
        --dry-run) dry=1; shift ;;
        -h | --help) usage; exit 0 ;;
        *) die "unknown option $1 (see --help)" 2 ;;
        esac
    done

    case "$prefix" in
    /*) ;;
    *) die "--prefix must be an absolute path" 2 ;;
    esac
    prefix=$(echo "$prefix" | sed 's|/*$||')
    case "$prefix" in
    "" | /usr | /usr/* | /etc | /etc/*) die "--prefix $prefix is not a place for this bundle (try /opt/kemu-agent)" 2 ;;
    *[!A-Za-z0-9/._-]*) die "--prefix may only contain letters, digits and / . _ -" 2 ;;
    esac
    if [ -n "$root" ]; then
        root=$(cd "$root" 2> /dev/null && pwd) || die "--root: no such directory" 2
        [ "$root" = / ] && root=
    fi

    # The bundle.
    [ -f "$here/BUNDLE-INFO" ] || die "BUNDLE-INFO is missing: run install.sh from an unpacked kemu-agent bundle"
    version=$(awk '$1 == "kemu-agent" { print $2 }' "$here/BUNDLE-INFO")
    arch=$(awk '$1 == "architecture" { print $2 }' "$here/BUNDLE-INFO")
    glibc=$(awk '$1 == "glibc" { print $2 }' "$here/BUNDLE-INFO")
    [ -n "$version" ] && [ -n "$arch" ] && [ -n "$glibc" ] || die "BUNDLE-INFO is incomplete"

    if command -v sha256sum > /dev/null 2>&1; then
        (cd "$here" && sha256sum -c --quiet SHA256SUMS) || die "the bundle is damaged: checksums do not match"
    else
        echo "sha256sum not found: the bundle's checksums are not checked." >&2
    fi

    bin="$here/usr/bin/kemu-agent"
    unit=$(find "$here" -name kemu-agent.service -type f | head -n 1)
    rules=$(find "$here" -name 70-kemu-agent.rules -type f | head -n 1)
    desktop=$(find "$here" -name org.kde.kemu.agent.desktop -type f | head -n 1)
    [ -f "$bin" ] && [ -n "$unit" ] && [ -n "$rules" ] && [ -n "$desktop" ] \
        && [ -f "$here/uninstall.sh" ] || die "the bundle is incomplete"

    # This system. With --root the target is not this system, so it is not
    # asked.
    if [ -z "$root" ]; then
        [ "$(uname -m)" = "$arch" ] || die "this bundle is for $arch, this system is $(uname -m)" 3
        sys_glibc=$(getconf GNU_LIBC_VERSION 2> /dev/null | awk '{ print $2 }') || sys_glibc=
        [ -n "$sys_glibc" ] || die "this system has no glibc; the bundle needs glibc $glibc or newer" 3
        version_ge "$sys_glibc" "$glibc" || die "this system has glibc $sys_glibc; the bundle needs $glibc or newer" 3
        [ "$dry" = 1 ] || [ "$(id -u)" = 0 ] || die "run it as root: sudo sh $0"
    fi

    # Another kemu-agent: a package or a source build under /usr, or a bundle
    # installed with another prefix.
    if [ "$force" = 0 ]; then
        for f in /usr/bin/kemu-agent /usr/local/bin/kemu-agent; do
            [ -e "$root$f" ] && die "kemu-agent is already installed as $f (a package or a build from source); remove it first, or use --force" 4
        done
        if [ -f "$root$unit_dir/kemu-agent.service" ] \
            && ! grep -qx "# Installed by kemu-agent's install.sh, prefix $prefix" "$root$unit_dir/kemu-agent.service"; then
            die "$unit_dir/kemu-agent.service belongs to another installation; uninstall that first, or use --force" 4
        fi
    fi

    old=
    [ -f "$root$prefix/INSTALLED" ] && old=$(cat "$root$prefix/INSTALLED")

    if [ -n "$old" ]; then
        installed=
        [ -f "$root$prefix/BUNDLE-INFO" ] \
            && installed=$(awk '$1 == "kemu-agent" { print $2 }' "$root$prefix/BUNDLE-INFO")
        if [ -z "$installed" ]; then
            echo "Replacing kemu-agent of unknown version in $prefix with $version"
        else
            case "$(version_cmp "$version" "$installed")" in
            1) echo "Upgrading kemu-agent in $prefix from $installed to $version" ;;
            0)
                if [ "$reinstall" = 0 ]; then
                    echo "kemu-agent $installed is installed in $prefix already; nothing to do (--reinstall installs it again)."
                    exit 0
                fi
                echo "Reinstalling kemu-agent $version in $prefix"
                ;;
            *)
                [ "$downgrade" = 1 ] \
                    || die "kemu-agent $installed in $prefix is newer than this bundle ($version); --allow-downgrade installs it anyway" 5
                echo "Downgrading kemu-agent in $prefix from $installed to $version"
                ;;
            esac
        fi
    else
        echo "Installing kemu-agent $version to $prefix"
    fi
    [ "$dry" = 1 ] && echo "(dry run: nothing is changed)"

    # Directories that exist already, from an earlier installation, stay
    # recorded as made by it.
    if [ -n "$old" ]; then
        list=$(echo "$old" | awk '$1 == "d"')
        [ -n "$list" ] && list="$list
"
    fi

    # The prefix must be writable; on image-based systems it may not be.
    if [ "$dry" = 0 ]; then
        dir=$prefix
        while [ ! -d "$root$dir" ]; do dir=$(dirname "$dir"); done
        { [ -w "$root$dir" ] && touch "$root$dir/.kemu-agent-write-test.$$" 2> /dev/null \
            && rm -f "$root$dir/.kemu-agent-write-test.$$"; } \
            || die "cannot write to $dir; choose a writable place with --prefix (on image-based systems e.g. /var/opt/kemu-agent)"
    fi
    mkdir_p "$prefix"

    put "$bin" "$prefix/bin/kemu-agent" 0755
    if [ -d "$here/usr/lib/kemu-agent" ]; then
        for lib in "$here/usr/lib/kemu-agent"/*; do
            [ -f "$lib" ] && put "$lib" "$prefix/lib/kemu-agent/${lib##*/}" 0644
        done
    fi
    put "$here/BUNDLE-INFO" "$prefix/BUNDLE-INFO" 0644
    put "$here/uninstall.sh" "$prefix/uninstall.sh" 0755
    put_rewritten "$unit" "$unit_dir/kemu-agent.service"
    put_link ../kemu-agent.service "$wants_dir/kemu-agent.service"
    put "$rules" "$rules_dir/70-kemu-agent.rules" 0644
    put_rewritten "$desktop" "$autostart_dir/org.kde.kemu.agent.desktop"
    record f "$prefix/INSTALLED"

    # Files of the old installation that the new one does not have.
    if [ -n "$old" ]; then
        echo "$old" | awk '$1 == "f" || $1 == "l" { print $2 }' | while read -r path; do
            case "$path" in "$prefix"/*) ;; *) continue ;; esac
            printf '%s' "$list" | awk -v p="$path" '$2 == p { found = 1 } END { exit !found }' && continue
            if [ "$dry" = 1 ]; then echo "  remove $path"; else rm -f "$root$path"; fi
        done
    fi

    if [ "$dry" = 0 ]; then
        printf '%s' "$list" | awk '!seen[$0]++' > "$root$prefix/INSTALLED.new.$$"
        mv -f "$root$prefix/INSTALLED.new.$$" "$root$prefix/INSTALLED"
    fi

    if [ -z "$root" ] && [ "$dry" = 0 ]; then
        activate
        [ -e "$port" ] || echo "Note: this virtual machine has no kemu-agent port ($port) yet; the agent starts once it has one and you log in again."
    fi

    echo "Done. To remove it: sudo sh $prefix/uninstall.sh"
}

main "$@"
exit 0
