#cloud-config
hostname: {{ HOSTNAME }}
manage_etc_hosts: true

users:
  - name: {{ USERNAME }}
    groups: [adm, sudo, wheel]
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

packages:
{% for pkg in PACKAGES %}
  - {{ pkg }}
{% endfor %}
package_update: true

growpart:
  mode: auto
  devices: ['/']

chpasswd:
  expire: false

runcmd:
  - truncate -c -s 0 /etc/machine-id
  - rm -f /var/lib/dbus/machine-id
  - cloud-init clean

power_state:
  delay: "+1"
  mode: poweroff
  message: "cloud-init configuration complete"
  condition: true
