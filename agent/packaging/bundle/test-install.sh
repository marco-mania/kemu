#!/bin/sh
# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
# SPDX-License-Identifier: GPL-3.0-or-later

# Tests install.sh and uninstall.sh against a system tree in a temporary
# directory (--root), with a bundle made from the agent's build output:
#
#   test-install.sh SHELL PROGRAM UNIT UDEV-RULE AUTOSTART-ENTRY
#
# SHELL runs the two scripts (sh, dash, bash). Run by ctest as
# agentbundleinstalltest.

set -eu

[ $# -eq 5 ] || { echo "usage: test-install.sh SHELL PROGRAM UNIT UDEV-RULE AUTOSTART-ENTRY" >&2; exit 2; }
shell=$1 program=$2 unit=$3 rules=$4 desktop=$5
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

failures=0
fail() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}
check() { # description command...
    desc=$1
    shift
    if "$@"; then echo "ok    $desc"; else fail "$desc"; fi
}
status_of() { # command... -> prints the exit status
    set +e
    "$@" > "$tmp/out" 2>&1
    echo $?
    set -e
}

# --- a bundle --------------------------------------------------------------

bundle=$tmp/bundle
# Laid out as the bundle has it: installed with the prefix /usr.
mkdir -p "$bundle/usr/bin" "$bundle/usr/lib/kemu-agent" "$bundle/usr/lib/systemd/user" \
    "$bundle/usr/lib/udev/rules.d" "$bundle/etc/xdg/autostart"
cp "$program" "$bundle/usr/bin/kemu-agent"
cp "$unit" "$bundle/usr/lib/systemd/user/kemu-agent.service"
mkdir "$bundle/usr/lib/systemd/user/graphical-session.target.wants"
ln -s ../kemu-agent.service "$bundle/usr/lib/systemd/user/graphical-session.target.wants/kemu-agent.service"
cp "$rules" "$bundle/usr/lib/udev/rules.d/70-kemu-agent.rules"
cp "$desktop" "$bundle/etc/xdg/autostart/org.kde.kemu.agent.desktop"
echo "first library" > "$bundle/usr/lib/kemu-agent/libfirst.so.1"
cp "$here/install.sh" "$here/uninstall.sh" "$bundle/"
bundle_version() { # version: sets the bundle's version, with fresh checksums
    printf 'kemu-agent %s\narchitecture test\nglibc 2.28 or newer\n' "$1" > "$bundle/BUNDLE-INFO"
    sums
}
sums() { (cd "$bundle" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > ../SHA256SUMS.new && mv ../SHA256SUMS.new SHA256SUMS); }
bundle_version 0.3.0

sys=$tmp/sys
mkdir -p "$sys/etc"

do_install() { status_of "$shell" "$bundle/install.sh" --root "$sys" "$@"; }
tree() { (cd "$sys" && find . \( -type f -o -type l \) | sed 's|^\./|/|' | sort); }

# --- first installation ----------------------------------------------------

check "install succeeds" [ "$(do_install)" = 0 ]
expected='/etc/systemd/user/graphical-session.target.wants/kemu-agent.service
/etc/systemd/user/kemu-agent.service
/etc/udev/rules.d/70-kemu-agent.rules
/etc/xdg/autostart/org.kde.kemu.agent.desktop
/opt/kemu-agent/BUNDLE-INFO
/opt/kemu-agent/INSTALLED
/opt/kemu-agent/bin/kemu-agent
/opt/kemu-agent/lib/kemu-agent/libfirst.so.1
/opt/kemu-agent/uninstall.sh'
check "installs exactly the expected files" [ "$(tree)" = "$expected" ]
check "program is executable" [ -x "$sys/opt/kemu-agent/bin/kemu-agent" ]
check "unit starts the installed program" grep -qx 'ExecStart=/opt/kemu-agent/bin/kemu-agent' "$sys/etc/systemd/user/kemu-agent.service"
check "unit carries the installation marker" grep -qx "# Installed by kemu-agent's install.sh, prefix /opt/kemu-agent" "$sys/etc/systemd/user/kemu-agent.service"
check "autostart entry starts the installed program" grep -qx 'Exec=/opt/kemu-agent/bin/kemu-agent' "$sys/etc/xdg/autostart/org.kde.kemu.agent.desktop"
check "unit is enabled for graphical sessions" [ "$(readlink "$sys/etc/systemd/user/graphical-session.target.wants/kemu-agent.service")" = ../kemu-agent.service ]
check "udev rule is the bundle's" cmp -s "$sys/etc/udev/rules.d/70-kemu-agent.rules" "$rules"

# --- upgrade ---------------------------------------------------------------

rm "$bundle/usr/lib/kemu-agent/libfirst.so.1"
echo "second library" > "$bundle/usr/lib/kemu-agent/libsecond.so.2"
bundle_version 0.4.0-rc1
check "upgrade succeeds" [ "$(do_install)" = 0 ]
check "upgrade says from which version" grep -q "from 0.3.0 to 0.4.0-rc1" "$tmp/out"
check "upgrade removes a library the new bundle lacks" [ ! -e "$sys/opt/kemu-agent/lib/kemu-agent/libfirst.so.1" ]
check "upgrade installs the new library" [ -f "$sys/opt/kemu-agent/lib/kemu-agent/libsecond.so.2" ]
check "INSTALLED has no duplicates" [ "$(sort "$sys/opt/kemu-agent/INSTALLED" | uniq -d)" = "" ]
check "INSTALLED forgets the removed library" [ "$(grep -c libfirst "$sys/opt/kemu-agent/INSTALLED" || true)" = 0 ]
check "installed BUNDLE-INFO is the new one" grep -qx "kemu-agent 0.4.0-rc1" "$sys/opt/kemu-agent/BUNDLE-INFO"

# --- versions --------------------------------------------------------------

rm "$sys/opt/kemu-agent/lib/kemu-agent/libsecond.so.2"
check "same version is left alone (status 0)" [ "$(do_install)" = 0 ]
check "same version says so" grep -q "installed in /opt/kemu-agent already" "$tmp/out"
check "same version changes nothing" [ ! -e "$sys/opt/kemu-agent/lib/kemu-agent/libsecond.so.2" ]
check "--reinstall installs it again" [ "$(do_install --reinstall)" = 0 ]
check "--reinstall restores a missing file" [ -f "$sys/opt/kemu-agent/lib/kemu-agent/libsecond.so.2" ]

bundle_version 0.3.0
check "older version is refused (status 5)" [ "$(do_install)" = 5 ]
check "older version keeps the installed one" grep -qx "kemu-agent 0.4.0-rc1" "$sys/opt/kemu-agent/BUNDLE-INFO"
check "--allow-downgrade installs it" [ "$(do_install --allow-downgrade)" = 0 ]
check "downgrade says so" grep -q "Downgrading kemu-agent in /opt/kemu-agent from 0.4.0-rc1 to 0.3.0" "$tmp/out"

bundle_version 0.4.0
check "release after its release candidate is an upgrade" [ "$(do_install)" = 0 ]
check "release replaces the candidate" grep -qx "kemu-agent 0.4.0" "$sys/opt/kemu-agent/BUNDLE-INFO"

# --- refusals --------------------------------------------------------------

before=$(cd "$sys" && find . | sort | xargs ls -ld --time-style=+%s 2> /dev/null | md5sum)
check "dry run succeeds" [ "$(do_install --dry-run --reinstall)" = 0 ]
check "dry run changes nothing" [ "$(cd "$sys" && find . | sort | xargs ls -ld --time-style=+%s 2> /dev/null | md5sum)" = "$before" ]

echo tampered >> "$bundle/usr/bin/kemu-agent"
check "damaged bundle is refused (status 1)" [ "$(do_install)" = 1 ]
check "damaged bundle is named as such" grep -q "damaged" "$tmp/out"
cp "$program" "$bundle/usr/bin/kemu-agent"

mkdir -p "$sys/usr/bin" && touch "$sys/usr/bin/kemu-agent"
check "kemu-agent in /usr is a conflict (status 4)" [ "$(do_install)" = 4 ]
check "--force overrides the conflict" [ "$(do_install --force --reinstall)" = 0 ]
rm -r "${sys:?}/usr"

check "another prefix is a conflict (status 4)" [ "$(do_install --prefix /var/opt/kemu-agent)" = 4 ]
check "relative prefix is wrong usage (status 2)" [ "$(do_install --prefix opt/kemu-agent)" = 2 ]
check "prefix under /usr is wrong usage (status 2)" [ "$(do_install --prefix /usr/lib/kemu-agent)" = 2 ]
check "unknown option is wrong usage (status 2)" [ "$(do_install --bogus)" = 2 ]

# --- uninstall -------------------------------------------------------------

do_uninstall() { status_of "$shell" "$sys/opt/kemu-agent/uninstall.sh" --root "$sys" "$@"; }

cp "$sys/opt/kemu-agent/INSTALLED" "$tmp/INSTALLED.saved"
echo "f /etc/passwd" >> "$sys/opt/kemu-agent/INSTALLED"
touch "$sys/etc/passwd"
check "altered INSTALLED is refused (status 1)" [ "$(do_uninstall)" = 1 ]
check "altered INSTALLED removes nothing" [ -f "$sys/etc/passwd" ]
check "altered INSTALLED leaves the program" [ -x "$sys/opt/kemu-agent/bin/kemu-agent" ]
cp "$tmp/INSTALLED.saved" "$sys/opt/kemu-agent/INSTALLED"
rm "$sys/etc/passwd"

check "uninstall dry run succeeds" [ "$(do_uninstall --dry-run)" = 0 ]
check "uninstall dry run changes nothing" [ -x "$sys/opt/kemu-agent/bin/kemu-agent" ]

check "uninstall succeeds" [ "$(do_uninstall)" = 0 ]
check "uninstall leaves no file behind" [ "$(tree)" = "" ]
check "uninstall removes the directories it made" [ "$(cd "$sys" && find . -mindepth 1 | sort | tr '\n' ' ')" = "./etc " ]

check "uninstall without installation succeeds" [ "$(status_of "$shell" "$bundle/uninstall.sh" --root "$sys")" = 0 ]

# A directory that existed before stays.
mkdir -p "$sys/etc/xdg/autostart" && touch "$sys/etc/xdg/autostart/other.desktop"
do_install > /dev/null
do_uninstall > /dev/null
check "directories that existed before stay" [ -f "$sys/etc/xdg/autostart/other.desktop" ]

if [ "$failures" -ne 0 ]; then
    echo "$failures check(s) failed ($shell)" >&2
    exit 1
fi
echo "all checks passed ($shell)"
