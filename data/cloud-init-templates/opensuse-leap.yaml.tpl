#cloud-config
hostname: {{ HOSTNAME }}
manage_etc_hosts: true

{% if DESKTOP_ENVIRONMENT %}
write_files:
  - path: /etc/default/grub.d/99-kemu-desktop.cfg
    permissions: '0644'
    content: |
      GRUB_TERMINAL_OUTPUT="gfxterm"
      GRUB_CMDLINE_LINUX_DEFAULT="quiet splash"
{% endif %}

users:
  - name: {{ USERNAME }}
    groups: [users, wheel]
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

package_update: true
package_upgrade: true

packages:
  - glibc-locale
{% for pkg in PACKAGES %}
  - {{ pkg }}
{% endfor %}
{% if DESKTOP_ENVIRONMENT %}
  - patterns-kde-kde_plasma
  - sddm
  - NetworkManager
  - dconf
{% endif %}

growpart:
  mode: auto
  devices: ['/']

chpasswd:
  expire: false

runcmd:
  - localectl set-locale LANG={{ LOCALE }}
  - localectl set-x11-keymap {{ KB_LAYOUT }} "" {{ KB_VARIANT }}

{% if DESKTOP_ENVIRONMENT %}
  - systemctl disable wicked.service
  - systemctl disable wickedd.service
  - systemctl enable NetworkManager.service
  - systemctl set-default graphical.target
  - systemctl enable sddm.service
  - rm -f /etc/default/grub.d/50-cloudimg-settings.cfg
  - grub2-mkconfig -o /boot/grub2/grub.cfg
{% endif %}

  - truncate -s 0 /etc/machine-id
  - rm -f /var/lib/dbus/machine-id

  - cloud-init clean

power_state:
  delay: "+1"
  mode: poweroff
  message: "cloud-init configuration complete"
  condition: true
