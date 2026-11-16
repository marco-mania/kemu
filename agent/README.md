# kemu-agent

The guest agent for Linux virtual machines run by KEMU. Running inside the user's graphical session, it:

- Shares the clipboard and primary selection with the host in all application-provided formats (text, HTML, images, custom formats).
- Reports the guest's monitor layout so the host can accurately map the pointer across displays regardless of per-monitor scaling.

The agent communicates with KEMU via a dedicated virtio-serial port (`org.kde.kemu.agent.0`) using the protocol specified in [docs/kemu-agent-protocol.md](../docs/kemu-agent-protocol.md). It replaces `spice-vdagent`, while `qemu-guest-agent` continues to run alongside it.

## Requirements

- **Runtime:** Qt 6.5 or later (Core, Gui, DBus) with the `xcb` and `offscreen` platform plugins (Qt Wayland support is not required), and `libwayland-client` 1.20 or later.
- **Build:** Extra CMake Modules (ECM), `wayland-scanner`, and `wayland-protocols`.

No KDE libraries are required at runtime, making the agent suitable for GNOME, Xfce, and other desktop environments in addition to Plasma.

## Building and installing

```bash
cmake -S agent -B build -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build
sudo cmake --install build
```

The agent resides in the `agent/` directory of KEMU's source tree and builds standalone without requiring KEMU's other sources or KDE Frameworks. To include it in KEMU's build for testing, configure KEMU with `-DKEMU_BUILD_AGENT=ON`. Because the agent belongs inside the guest, nothing is installed by default unless `-DKEMU_AGENT_INSTALL=ON` is also set.

Installation places the following files:

| File | Purpose |
| --- | --- |
| `/usr/bin/kemu-agent` | The agent binary |
| `/usr/lib/udev/rules.d/70-kemu-agent.rules` | Grants the active session user access to the virtio-serial port |
| `/usr/lib/systemd/user/kemu-agent.service` | Starts the agent with the graphical session |
| `/usr/lib/systemd/user/graphical-session.target.wants/kemu-agent.service` | Enables the service for all users (pass `-DKEMU_AGENT_ENABLE_USER_UNIT=OFF` to leave this to packaging) |
| `/etc/xdg/autostart/org.kde.kemu.agent.desktop` | Autostarts the agent in sessions without systemd user units (e.g., Xfce) |

After installation, reload udev rules (`sudo udevadm control --reload && sudo udevadm trigger`) or reboot the guest. If the virtio-serial port is absent, the agent exits silently, allowing the package to be safely installed on any system.

## How it works in the guest

| Session | Clipboard | Layout |
| --- | --- | --- |
| KDE Plasma (Wayland) | `ext-data-control` (or `wlr-data-control` on older KWin, e.g., Debian 13) | `xdg-output` |
| wlroots compositors (Sway, Hyprland, labwc) | `ext-data-control` or `wlr-data-control` | `xdg-output` |
| GNOME (Wayland) | X11 via Xwayland (Mutter lacks data-control) | `xdg-output` over its own Wayland connection |
| X11 desktops (Xfce, Cinnamon, MATE, Plasma X11) | X11 selections | RandR |

On Wayland, the agent manages its own connection to the compositor to speak `data-control` directly (preferring `ext-data-control`), while Qt runs on the `offscreen` platform. If the compositor lacks `data-control`, the agent falls back to X11 selections via Xwayland with Qt running on `xcb`. A session-wide `QT_QPA_PLATFORM=wayland` is overridden accordingly; other values are preserved.

You can check the selected backends in the user journal (`journalctl --user -u kemu-agent`), logged as `kemu-agent … - clipboard: … - layout: …`. For protocol-level logging, run `kemu-agent --verbose` or set `QT_LOGGING_RULES="org.kde.kemu.agent.debug=true"`.

Only one agent instance holds the port at a time. When switching user sessions, the outgoing session's agent disconnects and releases the port so the active session's agent can take over.

## Host-side requirements

No manual setup is required: KEMU automatically attaches the virtio-serial port alongside the `qemu-guest-agent` port for every VM using its integrated viewer and a PCI bus. Existing VMs receive it on their next start:

```text
-device virtio-serial-pci
-chardev socket,id=kemuagent,fd=<socketpair end>
-device virtserialport,chardev=kemuagent,id=kemuagent0,name=org.kde.kemu.agent.0
```

In KEMU's viewer, *Devices → Clipboard* controls the transfer direction. The status bar's clipboard indicator remains dimmed until an agent connects, after which it displays the agent's version.

When running a VM outside of KEMU, `tests/e2e/hostsim.py` can simulate the host (requires the `cbor2` Python module). Configure QEMU with a Unix socket (`-chardev socket,id=kemuagent,path=/tmp/kemu-agent.sock,server=on,wait=off`) and run:

```bash
python3 tests/e2e/hostsim.py /tmp/kemu-agent.sock
```

The simulator prints anything copied in the guest and offers each line typed into its console back to the guest clipboard.

## Known limitations (v0.3)

- **Clipboard size:** Host clipboard content is fetched immediately upon offer (up to 32 MiB per copy) and cached on the guest clipboard to serve pasting applications. Larger payloads remain on the host.
- **File transfers:** Files and folders are transferred only when *Share Files* is enabled in KEMU's viewer (disabled by default), up to the configured limit (256 MiB per copy by default). Items are copied into `~/.cache/kemu-agent/clipboard` immediately upon copying and cleaned up when the agent exits. Symlinks are skipped, and "cut" operations behave like "copy".
- **URI filtering:** When *Share Files* is disabled, file URIs are stripped from `text/uri-list`.
- **Display mapping:** Only a single `virtio-gpu` device is mapped to heads; any additional connectors are reported without a head mapping.
- **Platform support:** Linux only (though the protocol itself does not preclude Windows or BSD implementations).

## Tests

Build and run the unit tests:

```bash
cmake -S agent -B build -DBUILD_TESTING=ON && cmake --build build
QT_QPA_PLATFORM=offscreen ctest --test-dir build
```

When built as part of KEMU (`-DKEMU_BUILD_AGENT=ON`), these run alongside KEMU's test suite under the `agent…test` prefix.

The end-to-end tests run the agent against a host simulator over a PTY and require the `cbor2` Python module:

- `tests/e2e/e2e-wayland.py build/bin/kemu-agent`: Runs inside a headless Sway instance with two outputs, using `wl-copy` and `wl-paste` as guest clients.
- `tests/e2e/e2e-x11.py build/bin/kemu-agent build/bin/clipboardtool`: Runs on Xvfb, testing both an X11 session and a Wayland session without `data-control`.
