# KEMU in the Flatpak

The Flatpak bundles QEMU, swtpm and virtiofsd (`KEMU_SELF_CONTAINED`) because
applications in the sandbox cannot run host executables directly. KEMU detects
the sandbox and adjusts its behaviour accordingly. For testing, individual
features can also be disabled with `--disable-capability <name>`.

## Permissions

| Permission                                    | Purpose                                                        |
| --------------------------------------------- | -------------------------------------------------------------- |
| `--socket=wayland`, `--share=ipc`                 | Native Wayland viewer and QEMU's SDL window            |
| `--device=dri`                                | 3D acceleration (virglrenderer)                                |
| `--device=kvm`                                | Hardware virtualization                                        |
| `--share=network`                             | User-mode networking and NAT of virtual networks               |
| `--socket=pulseaudio`, `--filesystem=xdg-run/pipewire-0` | Guest audio                                         |
| `--filesystem=xdg-config/kdeglobals:ro`       | KDE colour scheme and fonts                                    |
| `--talk-name=org.kde.kwalletd6`               | Passwords of encrypted disks in KWallet                        |
| `--talk-name=org.kde.StatusNotifierWatcher`   | System tray icon                                               |

Notifications and application menu entries use desktop portals and require
no additional permissions.

## Granting more

Some permissions are intentionally omitted to preserve sandbox isolation.
Grant additional access only when needed:

- **USB passthrough:** Grant device access with
  `flatpak override --user --device=all org.kde.kemu`, in addition to the udev
  rule described in the README.
- **ISO images and shared folders outside the application data directory:**
  Select files through the file dialog (portal), or grant access to a folder,
  for example with
  `flatpak override --user --filesystem=xdg-download org.kde.kemu`.
- **Shutting down the host with a guest:** Grant
  `flatpak override --user --talk-name=org.kde.Shutdown org.kde.kemu` on Plasma,
  or use `--system-talk-name=org.freedesktop.login1` for logind access. Without
  either permission, this option is disabled.

## Differences

- **Executables:** *Settings → System → Executables* is read-only. Bundled
  programs cannot be replaced with host binaries. Paths configured in a native
  installation remain stored for later use.
- **Firmware:** The bundled QEMU includes UEFI firmware. You can still
  specify a firmware directory under *Settings → System → UEFI Firmware*.
- **Start scripts:** `qemu.sh` and *Show QEMU Command…* use paths inside the
  sandbox (`/app/bin/…`). Run the commands through `flatpak run --command=…`,
  not directly on the host.
- **File locations:** Data and configuration are stored under
  `~/.var/app/org.kde.kemu/`. VMs from a native installation are not detected
  automatically; select their directory under *Settings → Virtual Machines →
  Storage location*.
- **Application menu entries:** The desktop portal asks for confirmation
  when creating an entry and again if the VM is renamed.
- **Dolphin action:** *New Virtual Machine from This Image…* is unavailable
  by default because Flatpak does not export service menus. To add it
  manually, save the following file as
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

- **Notification settings:** System Settings does not list KEMU's Flatpak
  events. Configure them through *Settings → Configure Notifications…* in
  KEMU.
- **Command line:** `flatpak run org.kde.kemu --start <vm>`.
