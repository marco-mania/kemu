#cloud-config
hostname: {{ HOSTNAME }}
manage_etc_hosts: true

{% if DESKTOP_ENVIRONMENT %}
write_files:
  - path: /usr/local/etc/X11/xorg.conf.d/10-kemu-video.conf
    content: |
      Section "Device"
          Identifier "Card0"
          Driver "scfb"
      EndSection
  - path: /usr/local/etc/X11/xorg.conf.d/20-kemu-keyboard.conf
    content: |
      Section "InputClass"
          Identifier "KeyboardDefaults"
          MatchIsKeyboard "on"
          Option "XkbLayout" "{{ KB_LAYOUT }}"
          Option "XkbVariant" "{{ KB_VARIANT }}"
      EndSection
{% endif %}

users:
  - name: {{ USERNAME }}
{% if DESKTOP_ENVIRONMENT %}
    groups: [wheel, operator, video]
{% else %}
    groups: [wheel, operator]
{% endif %}
    shell: /bin/sh
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

packages:
  - sudo
{% for pkg in PACKAGES %}
  - {{ pkg }}
{% endfor %}
{% if DESKTOP_ENVIRONMENT %}
  - xorg
  - xfce
  - lightdm
  - lightdm-gtk-greeter
{% endif %}
package_update: true
package_upgrade: true

growpart:
  mode: auto
  devices: ['/']

chpasswd:
  expire: false

runcmd:
  - sysrc qemu_guest_agent_enable=YES
  - service qemu-guest-agent start
{% if DESKTOP_ENVIRONMENT %}
  - sysrc -f /boot/loader.conf hw.usb.usbhid.enable=1
  - sysrc dbus_enable=YES
  - sysrc lightdm_enable=YES
{% endif %}

power_state:
  delay: "+1"
  mode: poweroff
  message: "cloud-init configuration complete"
  condition: true
