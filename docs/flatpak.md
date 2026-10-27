# KEMU in the Flatpak

The Flatpak bundles its own QEMU, swtpm and virtiofsd (`KEMU_SELF_CONTAINED`),
because the sandbox cannot run programs of the host. KEMU detects the sandbox
and adjusts itself; features can also be switched off for testing with
`--disable-capability <name>`.

## Permissions

| Permission                                    | Purpose                                                        |
| --------------------------------------------- | -------------------------------------------------------------- |
| `--socket=wayland`, `--socket=fallback-x11`, `--share=ipc` | Viewer and QEMU's SDL window                      |
| `--device=dri`                                | 3D acceleration (virglrenderer)                                |
| `--device=kvm`                                | Hardware virtualization                                        |
| `--share=network`                             | User-mode networking and NAT of virtual networks               |
| `--socket=pulseaudio`, `--filesystem=xdg-run/pipewire-0` | Guest audio                                         |
| `--filesystem=xdg-config/kdeglobals:ro`       | KDE colour scheme and fonts                                    |
| `--talk-name=org.kde.kwalletd6`               | Passwords of encrypted disks in KWallet                        |
| `--talk-name=org.kde.StatusNotifierWatcher`   | System tray icon                                               |

Notifications and application menu entries go through the desktop portals and
need no permission of their own.

## Granting more

Some access is deliberately not granted, so the sandbox keeps meaning
something. Grant it when needed:

- **USB passthrough:** `flatpak override --user --device=all org.kde.kemu`, in
  addition to the udev rule in the README.
- **ISO images and shared folders** outside the app's data: pick files in the
  file dialog (portal), or grant a folder, e.g.
  `flatpak override --user --filesystem=xdg-download org.kde.kemu`.
- **Shutting down the host with a guest:**
  `flatpak override --user --talk-name=org.kde.Shutdown org.kde.kemu` (Plasma)
  or `--system-talk-name=org.freedesktop.login1`. Without either, the option is
  disabled.

## Differences

- **Executables:** *Settings → System → Executables* is read-only; the bundled
  programs cannot be replaced with host binaries. Paths configured in a native
  installation are kept for later.
- **Firmware:** The bundled QEMU brings UEFI firmware. A directory set under
  *Settings → System → UEFI Firmware* works as usual.
- **Start scripts:** `qemu.sh` and *Show QEMU Command…* refer to paths inside
  the sandbox (`/app/bin/…`). Run them with `flatpak run --command=…`, not on
  the host directly.
- **File locations:** Data and configuration live under
  `~/.var/app/org.kde.kemu/`. VMs of a native installation are not found
  automatically; point *Settings → Virtual Machines → Storage location* to them.
- **Application menu entries:** The desktop portal asks once per entry and
  installs it; after renaming a VM it asks again.
- **Dolphin action:** *New Virtual Machine from This Image…* is not available,
  because Flatpak does not export service menus. To add it, save this as
  `~/.local/share/kio/servicemenus/kemu-newvm.desktop`:

  ```ini
  [Desktop Entry]
  Type=Service
  MimeType=application/vnd.efi.iso;application/x-cd-image;
  X-KDE-Protocols=file
  X-KDE-MaxNumberOfUrls=1
  Actions=newVm;

  [Desktop Action newVm]
  Name=New Virtual Machine from This Image…
  Icon=org.kde.kemu
  Exec=flatpak run org.kde.kemu --new-vm %f
  ```

- **Notification settings:** System Settings does not list KEMU's events for
  the Flatpak; use *Settings → Configure Notifications…* in KEMU.
- **Command line:** `flatpak run org.kde.kemu --start <vm>`.
