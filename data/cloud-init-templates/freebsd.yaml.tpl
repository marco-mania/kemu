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
  - qemu-guest-agent
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
  # The agent talks through the virtio console driver.
  - sysrc -f /boot/loader.conf virtio_console_load=YES
  - sysrc qemu_guest_agent_enable=YES
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
  # poweroff. runcmd runs on every boot while /firstboot exists, and the
  # reboot firstboot_pkg_upgrade may have requested would beat the poweroff.
  - rm -f /firstboot /firstboot-reboot
  # Delayed so /etc/rc can finish; the forked shutdown must not hold the
  # pipe nuageinit tees runcmd output through, or /etc/rc blocks on it.
  - shutdown -p +1 < /dev/null > /dev/null 2>&1
