# OS and cloud-init templates

OS templates define the operating systems available when creating a VM, including hardware defaults, releases, and cloud images. Cloud-init templates (`.yaml.tpl`) generate the user data and network configuration for automated deployments.

## Managing templates

Under *Settings → Templates*, you can create, duplicate, customise, reset, hide, and share templates; changes take effect immediately.

- **Sharing:** *Export* creates a `.kemu-template.tar.gz` package containing `template.json` and its referenced cloud-init templates. *Import* accepts both packages and plain `.json` files (packages can also be opened directly from the file manager).
- **Cloud-init templates:** The *Cloud-Init Templates* tab lists available placeholders and previews rendered output using deployment defaults. Templates named `network-*` provide network configuration, while all others provide user data. The new VM dialog also lets you choose a different user-data template from the one set by the OS template.

## Where templates come from

`data/templates.json` defines global QEMU defaults, while `data/templates.d/` stores one JSON file per bundled template. Downloaded templates reside under `~/.local/share/kemu/ghns/` (one package per subdirectory). User templates in `~/.local/share/kemu/data/templates.d/` take precedence over both downloaded and bundled templates:

- A file overrides a bundled file with the same name; an empty file (or a symlink to `/dev/null`) hides it.
- A template with an existing `id` replaces that template completely.
- `sort_order` determines ordering within an OS group (bundled: `100`–`199`; default: `500`; "Other": `1000`).
- `cloud_init_template` and `network_template` reference `.yaml.tpl` files, resolved in order from the user's `cloud-init-templates/` directory, downloaded templates (including packages), and bundled templates.
- Templates listed in `~/.local/share/kemu/data/hidden-templates.json` are not offered for new VMs.

If no template is left to offer—none was found, for example after an incomplete installation, or all are hidden—the new VM dialog lists the operating system families instead and says why. The VM then gets the global defaults from `templates.json`, or KEMU's built-in ones if that file is missing too; installing from an ISO image or starting with an empty disk still works, automated installation does not.

## Format

Minimal example (`mydistro.json`):

```json
{
  "version": 1,
  "templates": [
    {
      "id": "mydistro",
      "display_name": "My Distro",
      "os_family": "linux",
      "cloud_init_template": "debian",
      "network_template": "network-v2",
      "releases": [
        {
          "version": "1.0",
          "cloud_images": [
            { "arch": "x86_64", "url": "[https://example.org/mydistro-1.0.qcow2](https://example.org/mydistro-1.0.qcow2)" }
          ]
        }
      ]
    }
  ]
}
```

Valid `os_family` values are `linux`, `windows`, `bsd`, `dos`, and `other`. Templates are loaded at startup; malformed files are skipped with a warning written to the debug log.

## Hardware defaults

`global_qemu_defaults` in `data/templates.json` sets the hardware of new VMs, and a template's `qemu_defaults` overrides single keys of it; the new VM dialog starts with RAM, CPU cores, and disk size from the result, which can be changed there. The keys and values are those of a VM's `config.json`. The defaults below are what `data/templates.json` sets, or KEMU's built-in value for keys it leaves out, such as the three for direct GPU memory access and Vulkan:

| Key                   | Values                                                                                                                                                              | Default      |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------ |
| `use_kvm`             | `true`, `false`                                                                                                                                                     | `true`       |
| `ram`                 | MiB                                                                                                                                                                 | `4096`       |
| `use_balloon`         | `true`, `false`                                                                                                                                                     | `true`       |
| `use_prealloc`        | `true`, `false`                                                                                                                                                     | `false`      |
| `cpu_model`           | `host`, `qemu64`, `qemu32`, `i486`, `pentium`, `pentium2`, `pentium3`, `pentium4`, `coreduo`, `core2duo`, `corei1`, `corei2`, `corei4`, `corei6`, `phenom`, `ryzen` | `host`       |
| `cpu_cores`           | Number of cores                                                                                                                                                     | `2`          |
| `disk_size_gb`        | GiB                                                                                                                                                                 | `40`         |
| `machine_type`        | `q35`, `pc`, `pc_isa`                                                                                                                                               | `q35`        |
| `gpu_type`            | `virtio`, `qxl`, `vga`, `vga_isa`, `cirrus`, `cirrus_isa`                                                                                                           | `virtio`     |
| `gpu_mem`             | MiB, fitted to the adapter ([display.md](display.md))                                                                                                               | `64`         |
| `monitor_count`       | `1`, `2`                                                                                                                                                            | `1`          |
| `gpu_3d_acceleration` | `true`, `false` (*3D*, VirtIO only)                                                                                                                                 | `true`       |
| `gpu_host_blobs`      | `true`, `false` (*Direct GPU memory access*, requires 3D)                                                                                                           | `false`      |
| `gpu_hostmem`         | MiB, a power of two from 256 to 8192; others round up                                                                                                               | `1024`       |
| `gpu_vulkan`          | `true`, `false` (*Vulkan*, requires direct GPU memory access)                                                                                                       | `false`      |
| `audio_device`        | `hda`, `ac97`, `es1370`, `sb16`, `adlib`, `pcspkr`, `none`                                                                                                          | `hda`        |
| `disk_interface`      | `virtio`, `nvme`, `sataide`                                                                                                                                         | `virtio`     |
| `net_card`            | `virtio`, `e1000`, `rtl8139`, `pcnet`, `rtl8029`, `ne2000`, `i82551`, `i82557b`, `i82559er`, `none`                                                                 | `virtio`     |
| `network_mode`        | `usermode`, `virtual_network`                                                                                                                                       | `usermode`   |
| `usb_controller`      | `xhci`, `ehci`, `default`, `none`                                                                                                                                   | `xhci`       |
| `pointer_device`      | `usb-tablet`, `usb-mouse`, `virtio-tablet`, `ps2`                                                                                                                   | `usb-tablet` |
| `keyboard_device`     | `ps2`, `usb`                                                                                                                                                        | `ps2`        |
| `use_uefi`            | `true`, `false`                                                                                                                                                     | `true`       |
| `use_secureboot`      | `true`, `false`                                                                                                                                                     | `false`      |
| `use_tpm`             | `true`, `false`                                                                                                                                                     | `false`      |
| `use_localtime`       | `true`, `false` (guest clock in local time, as Windows expects)                                                                                                     | `false`      |
| `use_acpi`            | `true`, `false`                                                                                                                                                     | `true`       |
| `guest_agent`         | `true`, `false` (QEMU guest agent channel)                                                                                                                          | `true`       |

- Unknown keys are ignored, and misspelled values are not rejected but fall back silently, for example to `vga` for `gpu_type`.
- Options the host cannot provide are kept in the new VM's configuration but take no effect when it starts; the VM settings turn them off once saved. Of the hardware, the new VM dialog only replaces a graphics adapter QEMU lacks and turns off 3D where QEMU has none.
- With `"audio_device": "sb16"`, an object `sb16_config` sets `irq`, `dma`, `dma16`, and `iobase` (defaults 5, 1, 5, and 0x220, the latter written as decimal `544`).
- Keys a VM gets when it is created, such as `name`, disk files, or `mac_address`, do not belong in a template.

Example: a template for a Linux distribution that starts with 3D, direct GPU memory access, and Vulkan:

```json
"qemu_defaults": {
  "gpu_type": "virtio",
  "gpu_3d_acceleration": true,
  "gpu_host_blobs": true,
  "gpu_hostmem": 4096,
  "gpu_vulkan": true
}
```
