# Shared folders

| Guest         | Protocol    | Host side   | Guest side              |
| ------------- | ----------- | ----------- | ----------------------- |
| **Linux/BSD** | 9p (VirtFS) | built in    | Kernel support          |
| **Windows**   | virtio-fs   | `virtiofsd` | VirtIO drivers + WinFsp |

The shared folder is set in the VM settings. Its *Shared Name* is the mount
tag in the guest. The examples use the tag `exchange` and the mount point
`/mnt/shared`.

## Linux

```sh
sudo mount -t 9p -o trans=virtio,version=9p2000.L exchange /mnt/shared
```

fstab:

```text
exchange /mnt/shared 9p trans=virtio,version=9p2000.L,rw,nofail 0 0
```

## FreeBSD 13+

```sh
sudo mount -t virtfs -o trans=virtio exchange /mnt/shared
```

fstab:

```text
exchange /mnt/shared virtfs trans=virtio,rw 0 0
```

OpenBSD and NetBSD support 9p only partly; check before relying on it.

## Mounting at login

Instead of fstab, an autostart entry can mount the share when the desktop
starts, e.g. `~/.config/autostart/mount-exchange.desktop`:

```ini
[Desktop Entry]
Type=Application
Name=Mount exchange
Exec=sudo mount -t 9p -o trans=virtio,version=9p2000.L exchange /home/myuser/exchange
```

This needs a sudo rule that allows the command without a password.

## Windows

KEMU starts `virtiofsd` on the host by itself. In the guest, the order of
installation matters:

1. Install [WinFsp](https://winfsp.dev/rel/).
2. Install the VirtIO guest tools (`virtio-win-gt-x64.msi`).
3. Reboot.

If the drive does not appear, check the service in PowerShell:

```powershell
Get-Service VirtioFsSvc                    # running?
Start-Service VirtioFsSvc                  # start it
sc.exe config VirtioFsSvc start=auto       # start it at boot
```
