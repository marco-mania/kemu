# kemu-agent

Guest agent for KEMU Linux virtual machines. Running inside the user's graphical session, it:

- Shares the clipboard and primary selection with the host in all application-provided formats (text, HTML, images, and custom formats)
- Reports the guest's monitor layout so the host can map the pointer accurately across displays regardless of per-monitor scaling

The agent communicates with KEMU over a dedicated virtio-serial port (`org.kde.kemu.agent.0`) using the protocol specified in [docs/kemu-agent-protocol.md](../docs/kemu-agent-protocol.md). It replaces `spice-vdagent` and runs alongside `qemu-guest-agent`.

## Requirements

- **Runtime:** Qt ≥ 6.5 (Core, Gui, DBus) with the `xcb` and `offscreen` platform plugins (Qt Wayland support is not needed), and `libwayland-client` ≥ 1.20
- **Build:** Extra CMake Modules (ECM), `wayland-scanner`, and `wayland-protocols`

No KDE libraries are required at runtime, so the agent works in GNOME, Xfce, and other desktop environments as well as Plasma.

## Building and (un)installing

```bash
cmake -S agent -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
sudo cmake --build build --target uninstall    # to remove it again
```

Located in `agent/` within KEMU's source tree, the agent builds on its own, without KEMU's other sources or KDE Frameworks; KEMU's build does not include it. Distributions can package it from the same source tarball as KEMU.

Installation places the following files:

| File                                                                      | Purpose                                                                                                  |
| ------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------- |
| `/usr/bin/kemu-agent`                                                     | Agent binary                                                                                             |
| `/usr/lib/udev/rules.d/70-kemu-agent.rules`                               | Grants the active session user access to the virtio-serial port                                          |
| `/usr/lib/systemd/user/kemu-agent.service`                                | Starts the agent with the graphical session                                                              |
| `/usr/lib/systemd/user/graphical-session.target.wants/kemu-agent.service` | Enables the service for all users (pass `-DKEMU_AGENT_ENABLE_USER_UNIT=OFF` to leave this to packaging) |
| `/etc/xdg/autostart/org.kde.kemu.agent.desktop`                           | Autostarts the agent in sessions without systemd user units (e.g., Xfce)                                 |

After installation, reload udev rules (`sudo udevadm control --reload && sudo udevadm trigger`) or reboot the guest. If the virtio-serial port is absent, the agent exits silently, making it safe to install on any system.

## Portable bundle

Where building on the guest is undesirable or impractical, the agent can be packaged once into a self-contained bundle. It links against an older glibc (2.28 by default—supporting RHEL 8, Debian 10, Ubuntu 18.10, and newer), statically embeds a minimal Qt, and bundles remaining non-glibc dependencies (such as `libxcb-cursor` for Qt 6.5+), so the guest needs neither Qt nor development packages.

The bundle has a build of its own in `packaging/bundle/`, which needs only CMake and a container engine; it installs the bundle to `share/kemu/agent/<arch>/`, where KEMU finds it to offer the agent to guests (see KEMU's README, "kemu-agent bundle for guests"):

```bash
cmake -S agent/packaging/bundle -B build-bundle
cmake --build build-bundle
sudo cmake --install build-bundle                    # or, without root: --prefix ~/.local
sudo cmake --build build-bundle --target uninstall   # to remove it again
```

Without CMake, `packaging/bundle/build-bundle.sh` builds a bundle as well (`--arch`, `--glibc`, `--out`; `--help` lists all options).

Building requires Podman or Docker. It builds and caches a `manylinux`-based toolchain image (the initial Qt compilation takes a while), compiles the agent inside it, and outputs `kemu-agent-<version>-linux-<arch>.tar.gz` alongside a `.sha256` file in the build directory.

- `"-DKEMU_AGENT_BUNDLE_ARCHS=x86_64;aarch64"`: Builds a bundle for each listed architecture (default: the host's). On `x86_64`, `aarch64` is not cross-compiled: the `aarch64` container runs natively under QEMU user-mode emulation (`binfmt`), so the first Qt build takes hours. KEMU's README ("Guests of other architectures") lists what the host needs.
- `-DKEMU_AGENT_BUNDLE_GLIBC=2.34`: Raises baseline compatibility to RHEL 9 and newer.

The archive mirrors the target guest layout (`usr/bin/kemu-agent`, bundled libraries in `usr/lib/kemu-agent/`, the udev rule, systemd user unit, and autostart entry) plus `install.sh`, `uninstall.sh`, `BUNDLE-INFO`, `MANIFEST`, and `SHA256SUMS`. During the build—or manually on an unpacked bundle—`packaging/bundle/check-bundle.sh` verifies the output and fails if any binary requires a newer glibc, a dynamic `libstdc++`, or an unbundled library.

### Installing the bundle

Unpack the archive in the guest and run as root:

```bash
sudo sh install.sh                          # installs to /opt/kemu-agent
sudo sh /opt/kemu-agent/uninstall.sh        # removes it again
```

`install.sh` first verifies the checksums, architecture, and glibc version. It places the program under `/opt/kemu-agent` and the user unit, udev rule, and autostart entry under `/etc` (`/etc/systemd/user`, `/etc/udev/rules.d`, `/etc/xdg/autostart`), pointing the unit and autostart entry to the installed program. Nothing goes under `/usr`, so the bundle neither conflicts with distribution packages nor requires a writable `/usr`; on image-based systems where `/opt` is read-only as well, choose another location with `--prefix` (e.g., `/var/opt/kemu-agent`). Afterwards, it reloads the udev rules and starts the agent in running graphical sessions. Running a newer bundle's `install.sh` upgrades the installation in place; for the version already installed it does nothing (`--reinstall` installs it again), and it refuses an older one unless `--allow-downgrade` is given (exit status 5).

`install.sh` refuses to install if `kemu-agent` is already installed another way—as a package or from source under `/usr`, or from a bundle with a different prefix—unless `--force` is given. Use `--dry-run` to preview changes, `--no-start` to leave running sessions untouched, and `--help` to list all options, including exit statuses for automated installation.

`install.sh` records every created file and directory in `/opt/kemu-agent/INSTALLED`. `uninstall.sh` stops the agent in running sessions and removes those exact entries (directories only if empty), refusing a list containing paths `install.sh` could not have created.

## How it works in the guest

| Session                                         | Clipboard                                                                 | Layout                                       |
| ----------------------------------------------- | ------------------------------------------------------------------------- | -------------------------------------------- |
| KDE Plasma (Wayland)                            | `ext-data-control` (or `wlr-data-control` on older KWin, e.g., Debian 13) | `xdg-output`                                 |
| wlroots compositors (Sway, Hyprland, labwc)     | `ext-data-control` or `wlr-data-control`                                  | `xdg-output`                                 |
| GNOME (Wayland)                                 | X11 via Xwayland (Mutter lacks data-control)                              | `xdg-output` over its own Wayland connection |
| X11 desktops (Xfce, Cinnamon, MATE, Plasma X11) | X11 selections                                                            | RandR                                        |

On Wayland, the agent manages its own connection to the compositor to speak `data-control` directly (preferring `ext-data-control`), while Qt runs on the `offscreen` platform. If the compositor lacks `data-control`, the agent falls back to X11 selections via Xwayland with Qt running on `xcb`. A session-wide `QT_QPA_PLATFORM=wayland` is overridden accordingly; other values are preserved.

Check the selected backends in the user journal (`journalctl --user -u kemu-agent`), logged as `kemu-agent … - clipboard: … - layout: …`. For protocol-level logging, run `kemu-agent --verbose` or set `QT_LOGGING_RULES="org.kde.kemu.agent.debug=true"`.

Only one agent instance holds the port at a time. When switching user sessions, the outgoing session's agent disconnects and releases the port so the active session's agent can take over.

## Host-side requirements

No manual setup is required: KEMU automatically attaches the virtio-serial port alongside the `qemu-guest-agent` port for every VM using the integrated viewer and a PCI bus (existing VMs receive it on their next start):

```text
-device virtio-serial-pci
-chardev socket,id=kemuagent,fd=<socketpair end>
-device virtserialport,chardev=kemuagent,id=kemuagent0,name=org.kde.kemu.agent.0
```

In KEMU's viewer, *Devices → Clipboard* controls the transfer direction. The status bar's clipboard indicator remains dimmed until an agent connects, then displays the agent's version.

When running a VM outside KEMU, `tests/e2e/hostsim.py` can simulate the host (requires the `cbor2` Python module). Configure QEMU with a Unix socket (`-chardev socket,id=kemuagent,path=/tmp/kemu-agent.sock,server=on,wait=off`) and run:

```bash
python3 tests/e2e/hostsim.py /tmp/kemu-agent.sock
```

The simulator prints anything copied in the guest and offers each line typed into its console back to the guest clipboard.

## Known limitations (v0.3)

- **Clipboard size:** Host clipboard content is fetched immediately upon offer (up to 32 MiB per copy) and cached in the guest to serve pasting applications; larger payloads remain on the host.
- **File transfers:** Files and folders transfer only when *Share Files* is enabled in KEMU's viewer (disabled by default), up to the configured limit (256 MiB per copy by default). Items are copied into `~/.cache/kemu-agent/clipboard` upon copy and cleaned up when the agent exits. Symlinks are skipped, and "cut" operations behave like "copy".
- **URI filtering:** When *Share Files* is disabled, file URIs are stripped from `text/uri-list`.
- **Display mapping:** Only a single `virtio-gpu` device is mapped to heads; additional connectors are reported without head mapping.
- **Platform support:** Linux only (though the protocol does not preclude Windows or BSD implementations).

## Tests

Build and run the unit tests:

```bash
cmake -S agent -B build -DBUILD_TESTING=ON && cmake --build build
QT_QPA_PLATFORM=offscreen ctest --test-dir build
```

They are not part of KEMU's test suite: run them in a build of their own, as above.

`agentbundleinstalltest` tests the bundle's `install.sh` and `uninstall.sh` against a system tree in a temporary directory (`--root`) using `sh` and, where available, `dash`.

End-to-end tests run the agent against a host simulator over a PTY and require the `cbor2` Python module:

- `tests/e2e/e2e-wayland.py build/bin/kemu-agent`: Runs inside a headless Sway instance with two outputs, using `wl-copy` and `wl-paste` as guest clients.
- `tests/e2e/e2e-x11.py build/bin/kemu-agent build/bin/clipboardtool`: Runs on Xvfb, testing both an X11 session and a Wayland session without `data-control`.
