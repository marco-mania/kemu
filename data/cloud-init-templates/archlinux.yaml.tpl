#cloud-config
hostname: {{ HOSTNAME }}
manage_etc_hosts: true

users:
  - name: {{ USERNAME }}
    groups: [adm, wheel]
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
{% if KB_VARIANT %}
  variant: {{ KB_VARIANT }}
{% endif %}

{% if PACKAGES or DESKTOP_ENVIRONMENT %}
packages:
{% endif %}
{% for pkg in PACKAGES %}
  - {{ pkg }}
{% endfor %}
{% if DESKTOP_ENVIRONMENT %}
  - plasma-desktop
  - sddm
  - sddm-kcm
  - networkmanager
  - plasma-nm
  - plasma-pa
  - xdg-desktop-portal-kde
  - powerdevil
  - kinfocenter
  - kdeplasma-addons
  - dolphin
  - dolphin-plugins
  - konsole
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
  - echo "LANG={{ LOCALE }}" > /etc/locale.conf
  - echo "KEYMAP={{ KB_LAYOUT }}" > /etc/vconsole.conf
  - localectl set-x11-keymap {{ KB_LAYOUT }} "" {{ KB_VARIANT }}
{% if DESKTOP_ENVIRONMENT %}
  - systemctl set-default graphical.target
  - systemctl disable systemd-networkd.service systemd-networkd.socket systemd-networkd-wait-online.service
  - systemctl enable NetworkManager.service
  - systemctl enable sddm.service
  - sed -i 's/GRUB_CMDLINE_LINUX_DEFAULT=\"\"/GRUB_CMDLINE_LINUX_DEFAULT=\"quiet splash\"/g' /etc/default/grub
  - grub-mkconfig -o /boot/grub/grub.cfg
{% endif %}
  - truncate -c -s 0 /etc/machine-id
  - rm -f /var/lib/dbus/machine-id
  - cloud-init clean

power_state:
  delay: "+1"
  mode: poweroff
  message: "cloud-init configuration complete"
  condition: true
