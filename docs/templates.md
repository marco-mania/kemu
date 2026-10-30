# OS and cloud-init templates

OS templates define the operating systems available when creating a VM,
including hardware defaults, releases and cloud images. Cloud-init templates
(`.yaml.tpl`) generate the user data and network configuration for automated
deployments.

## Managing templates

Under *Settings → Templates*, you can create, duplicate, customise, reset,
hide and share templates. Changes take effect immediately.

- **Sharing:** *Export* creates a `.kemu-template.tar.gz` package containing
  `template.json` and the cloud-init templates it uses. *Import* accepts
  packages and plain `.json` files. Packages can also be opened from the file
  manager.
- **Cloud-init templates:** The *Cloud-Init Templates* tab lists available
  placeholders and previews the rendered output using deployment defaults.
  Templates named `network-*` provide network configuration; all others
  provide user data. The new VM dialog also lets you choose a different
  user-data template from the one defined by the OS template.

## Where templates come from

`data/templates.json` defines global QEMU defaults, while
`data/templates.d/` contains one JSON file per bundled template. Downloaded
templates are stored under `~/.local/share/kemu/ghns/`, with one package per
subdirectory. User templates in `~/.local/share/kemu/data/templates.d/` take
precedence over both downloaded and bundled templates:

- A file overrides a bundled file with the same name. An empty file (or a
  symlink to `/dev/null`) hides it.
- A template with an existing `id` replaces that template completely.
- `sort_order` determines the position within an OS group (bundled:
  100–199; default: 500; "Other": 1000).
- `cloud_init_template` and `network_template` reference `.yaml.tpl` files.
  KEMU searches the user's `cloud-init-templates/` directory first, followed
  by downloaded templates (including packages), then bundled templates.
- Templates listed in `~/.local/share/kemu/data/hidden-templates.json` are not
  offered for new VMs.

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
            { "arch": "x86_64", "url": "https://example.org/mydistro-1.0.qcow2" }
          ]
        }
      ]
    }
  ]
}
```

Valid `os_family` values are `linux`, `windows`, `bsd`, `dos` and `other`.
Templates are loaded at startup. Malformed files are skipped and a warning
is written to the debug log.
