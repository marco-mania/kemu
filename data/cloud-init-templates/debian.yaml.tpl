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
  - task-desktop
  - dconf-cli
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
  - update-grub
  - DEBIAN_FRONTEND=noninteractive apt-get install -y plymouth plymouth-themes
# GNOME keyboard settings
  - mkdir -p /etc/dconf/db/local.d
  - echo "user-db:user" > /etc/dconf/profile/user
  - echo "system-db:local" >> /etc/dconf/profile/user
  - |
    if [ -n "{{ KB_VARIANT }}" ]; then
      GNOME_LAYOUT="{{ KB_LAYOUT }}+{{ KB_VARIANT }}"
    else
      GNOME_LAYOUT="{{ KB_LAYOUT }}"
    fi
    echo "[org/gnome/desktop/input-sources]" > /etc/dconf/db/local.d/00-keyboard
    echo "sources=[('xkb', '$GNOME_LAYOUT')]" >> /etc/dconf/db/local.d/00-keyboard
  - dconf update
# END GNOME keyboard settings
{% endif %}
  - truncate -c -s 0 /etc/machine-id
  - rm -f /var/lib/dbus/machine-id
  - cloud-init clean

power_state:
  delay: "+1"
  mode: poweroff
  message: "cloud-init configuration complete"
  condition: true
