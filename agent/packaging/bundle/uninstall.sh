#!/bin/sh
# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
# SPDX-License-Identifier: GPL-3.0-or-later

# Removes kemu-agent installed with install.sh:
#
#   sudo sh /opt/kemu-agent/uninstall.sh [--prefix DIR] [--dry-run]
#
# It stops the agent in the running sessions and removes exactly the files
# and directories install.sh recorded in DIR/INSTALLED; directories only when
# they are empty. Run from the installation, it finds DIR itself; run from a
# bundle, DIR is /opt/kemu-agent unless --prefix says otherwise.
#
# Options:
#   --prefix DIR   the installation to remove
#   --dry-run      say what would be removed, change nothing
#   --root DIR     remove from the system tree below DIR (for images and
#                  tests); nothing is stopped or reloaded then
#
# Exit status: 0 done (also when nothing was installed), 1 failed, 2 wrong
# usage.

set -eu

prefix=
root=
dry=0

unit_dir=/etc/systemd/user
wants_dir=/etc/systemd/user/graphical-session.target.wants
rules_dir=/etc/udev/rules.d
autostart_dir=/etc/xdg/autostart

here=$(cd "$(dirname "$0")" && pwd)

die() { # message [status]
    echo "uninstall.sh: $1" >&2
    exit "${2:-1}"
}

usage() {
    sed -n '5,21p' "$0" | sed 's/^# \{0,1\}//'
}

# allowed TYPE PATH: whether install.sh can have made PATH. Anything else in
# INSTALLED is refused, so that a damaged or altered list cannot remove other
# files.
allowed() {
    case "$2" in *..* | *//* | *[!A-Za-z0-9/._-]*) return 1 ;; esac
    case "$1:$2" in
    [fl]:"$prefix"/*) return 0 ;;
    f:"$unit_dir/kemu-agent.service" | l:"$wants_dir/kemu-agent.service") return 0 ;;
    f:"$rules_dir/70-kemu-agent.rules" | f:"$autostart_dir/org.kde.kemu.agent.desktop") return 0 ;;
    d:"$prefix" | d:"$prefix"/*) return 0 ;;
    d:/opt | d:/var/opt | d:/etc/systemd | d:"$unit_dir" | d:"$wants_dir") return 0 ;;
    d:/etc/udev | d:"$rules_dir" | d:/etc/xdg | d:"$autostart_dir") return 0 ;;
    esac
    # A prefix's missing parents, as install.sh creates them.
    case "$1" in d) case "$prefix/" in "$2"/*) return 0 ;; esac ;; esac
    return 1
}

# for_each_session_user SCRIPT: runs SCRIPT with sh as each user with a
# running user manager; $1 is the user's name.
for_each_session_user() {
    [ -d /run/systemd/system ] || return 0
    for dir in /run/user/[0-9]*; do
        [ -S "$dir/bus" ] || continue
        uid=${dir##*/}
        [ "$uid" -ge 1000 ] 2> /dev/null || continue
        name=$(id -nu "$uid" 2> /dev/null) || continue
        runuser -u "$name" -- env XDG_RUNTIME_DIR="$dir" \
            DBUS_SESSION_BUS_ADDRESS="unix:path=$dir/bus" \
            sh -c "$1" kemu-agent-uninstall "$name" || true
    done
}

main() {
    while [ $# -gt 0 ]; do
        case "$1" in
        --prefix) [ $# -ge 2 ] || die "--prefix needs a directory" 2; prefix=$2; shift 2 ;;
        --prefix=*) prefix=${1#*=}; shift ;;
        --root) [ $# -ge 2 ] || die "--root needs a directory" 2; root=$2; shift 2 ;;
        --root=*) root=${1#*=}; shift ;;
        --dry-run) dry=1; shift ;;
        -h | --help) usage; exit 0 ;;
        *) die "unknown option $1 (see --help)" 2 ;;
        esac
    done

    if [ -n "$root" ]; then
        root=$(cd "$root" 2> /dev/null && pwd) || die "--root: no such directory" 2
        [ "$root" = / ] && root=
    fi
    if [ -z "$prefix" ]; then
        # Run from the installation itself (with --root it lives below it).
        if [ -f "$here/INSTALLED" ]; then
            prefix=${here#"$root"}
        else
            prefix=/opt/kemu-agent
        fi
    fi
    case "$prefix" in /*) ;; *) die "--prefix must be an absolute path" 2 ;; esac
    prefix=$(echo "$prefix" | sed 's|/*$||')
    case "$prefix" in
    "" | /usr | /usr/* | /etc | /etc/*) die "--prefix $prefix is not an installation of this bundle" 2 ;;
    *[!A-Za-z0-9/._-]*) die "--prefix may only contain letters, digits and / . _ -" 2 ;;
    esac

    installed="$root$prefix/INSTALLED"
    if [ ! -f "$installed" ]; then
        echo "kemu-agent is not installed in $prefix (no $prefix/INSTALLED)."
        exit 0
    fi
    [ -n "$root" ] || [ "$dry" = 1 ] || [ "$(id -u)" = 0 ] || die "run it as root: sudo sh $0"

    # Check the whole list before removing anything.
    bad=$(while read -r type path; do
        [ -n "$type" ] || continue
        allowed "$type" "$path" || echo "$type $path"
    done < "$installed")
    [ -z "$bad" ] || die "$installed lists paths install.sh does not make; nothing removed:
$bad"

    echo "Removing kemu-agent from $prefix"
    [ "$dry" = 1 ] && echo "(dry run: nothing is changed)"

    if [ -z "$root" ] && [ "$dry" = 0 ]; then
        # $1 is expanded by the user's shell, not here.
        # shellcheck disable=SC2016
        for_each_session_user 'systemctl --user stop kemu-agent.service 2> /dev/null && echo "Stopped kemu-agent for $1." || true'
    fi

    # Files and links first, then the directories, deepest first. INSTALLED
    # goes last, so that an interrupted run can be repeated.
    while read -r type path; do
        case "$type" in f | l) ;; *) continue ;; esac
        [ "$path" = "$prefix/INSTALLED" ] && continue
        [ -e "$root$path" ] || [ -L "$root$path" ] || continue
        if [ "$dry" = 1 ]; then echo "  remove $path"; else rm -f "$root$path"; fi
    done < "$installed"
    if [ "$dry" = 1 ]; then
        echo "  remove $prefix/INSTALLED"
    else
        dirs=$(awk '$1 == "d" { print $2 }' "$installed" | sort -r)
        rm -f "$installed"
        for dir in $dirs; do
            rmdir "$root$dir" 2> /dev/null || true
        done
    fi
    [ "$dry" = 1 ] && awk '$1 == "d" { print "  rmdir (if empty) " $2 }' "$installed" | sort -r

    if [ -z "$root" ] && [ "$dry" = 0 ]; then
        for_each_session_user 'systemctl --user daemon-reload'
        if command -v udevadm > /dev/null 2>&1 && [ -d /run/udev ]; then
            udevadm control --reload || true
        fi
    fi
    echo "Done."
}

main "$@"
exit 0
