#cloud-config
hostname: {{ HOSTNAME }}
manage_etc_hosts: true

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
    passwd: {{ PASSWORD_HASH }}
{% else %}
    # nuageinit ignores lock_passwd and would leave the password empty.
    passwd: "*"
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
{% if DESKTOP_ENVIRONMENT %}
  - sysrc -f /boot/loader.conf hw.usb.usbhid.enable=1
  - sysrc dbus_enable=YES
  - sysrc lightdm_enable=YES
  - mkdir -p /usr/local/etc/X11/xorg.conf.d
  - |
    cat > /usr/local/etc/X11/xorg.conf.d/10-kemu-video.conf <<'EOF'
    Section "Device"
        Identifier "Card0"
        Driver "scfb"
    EndSection
    EOF
  - |
    cat > /usr/local/etc/X11/xorg.conf.d/20-kemu-keyboard.conf <<'EOF'
    Section "InputClass"
        Identifier "KeyboardDefaults"
        MatchIsKeyboard "on"
        Option "XkbLayout" "{{ KB_LAYOUT }}"
        Option "XkbVariant" "{{ KB_VARIANT }}"
    EndSection
    EOF
{% endif %}
  # nuageinit in FreeBSD 14.x/15.1 ignores power_state; KEMU waits for the
  # poweroff. Delayed so /etc/rc can finish and clear /firstboot; the reboot
  # firstboot_pkg_upgrade may have requested would otherwise win.
  - rm -f /firstboot-reboot
  - shutdown -p +1
