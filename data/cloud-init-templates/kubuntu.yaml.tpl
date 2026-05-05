#cloud-config
hostname: {{ HOSTNAME }}
manage_etc_hosts: true

write_files:
{% if DESKTOP_ENVIRONMENT %}
  - path: /etc/default/grub.d/99-kemu-desktop.cfg
    content: |
      GRUB_TERMINAL_OUTPUT="gfxterm"
      GRUB_CMDLINE_LINUX_DEFAULT="quiet splash"
      GRUB_SERIAL_COMMAND=""
  - path: /var/lib/AccountsService/users/{{ USERNAME }}
    content: |
      [User]
      Language={{ LOCALE }}
      XSession=plasma
      Session=plasma
{% endif %}

users:
  - name: {{ USERNAME }}
    groups: [adm, sudo]
    shell: /bin/bash
    sudo: ALL=(ALL) NOPASSWD:ALL
{% if PASSWORD_HASH %}
    lock_passwd: false
    passwd: {{ PASSWORD_HASH }}
{% else %}
    lock_passwd: true
{% endif %}
{% if SSH_KEYS %}
    ssh_authorized_keys:
{% for key in SSH_KEYS %}
      - {{ key }}
{% endfor %}
{% endif %}

ssh_pwauth: {{ SSH_PWAUTH }}
timezone: {{ TIMEZONE }}
locale: {{ LOCALE }}

keyboard:
  layout: {{ KB_LAYOUT }}
  variant: {{ KB_VARIANT }}

debconf_selections: |
  keyboard-configuration keyboard-configuration/layout select {{ KB_LAYOUT }}
  keyboard-configuration keyboard-configuration/variant select {{ KB_VARIANT }}
  sddm shared/default-x-display-manager select sddm

apt:
  conf: |
    Acquire::Languages "{{ LANG }}";

packages:
  - console-setup
  - keyboard-configuration
{% for pkg in PACKAGES %}
  - {{ pkg }}
{% endfor %}
{% if DESKTOP_ENVIRONMENT %}
  - kubuntu-desktop
{% endif %}
package_update: true
package_upgrade: true

growpart:
  mode: auto
  devices: ['/']

chpasswd:
  expire: false

runcmd:
  - sed -i 's/^#\s*\({{ LOCALE }}\)/\1/' /etc/locale.gen
  - locale-gen
  - update-locale LANG={{ LOCALE }} LANGUAGE={{ LANG }}
  - localectl set-x11-keymap {{ KB_LAYOUT }} "" {{ KB_VARIANT }}
  - DEBIAN_FRONTEND=noninteractive dpkg-reconfigure keyboard-configuration
{% if DESKTOP_ENVIRONMENT %}
  - systemctl set-default graphical.target
  - rm -f /etc/default/grub.d/50-cloudimg-settings.cfg
  - rm -f /etc/default/grub.d/50-cloudimg-serial.cfg
  - update-grub
  # Make SDDM the default display manager
  - echo "/usr/bin/sddm" > /etc/X11/default-display-manager
  - DEBIAN_FRONTEND=noninteractive dpkg-reconfigure -fnoninteractive sddm || true
  # Systemwide Plasma keyboard layout (read by kxkbrc on first user login)
  - mkdir -p /etc/xdg
  - |
    cat > /etc/xdg/kxkbrc <<EOF
    [Layout]
    LayoutList={{ KB_LAYOUT }}
    VariantList={{ KB_VARIANT }}
    Model=pc105
    Use=true
    EOF
{% endif %}
  - truncate -c -s 0 /etc/machine-id
  - rm -f /var/lib/dbus/machine-id
  - cloud-init clean

power_state:
  delay: "+1"
  mode: poweroff
  message: "cloud-init configuration complete"
  condition: true
