# OS and cloud-init templates

OS templates describe the operating systems offered for new VMs: hardware
defaults, releases and their cloud images. Cloud-init templates (`.yaml.tpl`)
produce the user data and network configuration of an automated deployment.

## Managing templates

Under *Settings → Templates* templates can be created, duplicated, customized,
reset, hidden and shared. Changes take effect immediately.

- **Sharing:** *Export* writes a package (`.kemu-template.tar.gz`) with
  `template.json` and the cloud-init templates it uses; *Import* reads packages
  and plain `.json` files. Packages also open from the file manager.
- **Cloud-init templates:** The *Cloud-Init Templates* tab lists the available
  placeholders and shows a preview rendered from the deployment defaults.
  Templates named `network-*` provide the network configuration, all others
  the user data. The new VM dialog can choose a user-data template other than
  the one of the OS template.

## Where templates come from

`data/templates.json` holds the global QEMU defaults, `data/templates.d/` one
JSON file per shipped template. Downloaded templates
(`~/.local/share/kemu/ghns/`, one package per subdirectory) come on top of the
shipped ones, and the user's own templates in
`~/.local/share/kemu/data/templates.d/` on top of both:

- A file replaces the shipped file of the same name; an empty file (or a
  symlink to `/dev/null`) hides it.
- A template with an existing `id` replaces that template completely.
- `sort_order` sets the position within the OS group (shipped: 100–199,
  default: 500, "Other": 1000).
- `cloud_init_template` and `network_template` name a `.yaml.tpl` file, looked
  up in the user's `cloud-init-templates/` first, then among the downloaded
  ones (including packages), then among the shipped ones.
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
Templates are read at startup; malformed files are skipped with a warning in
the debug log.
