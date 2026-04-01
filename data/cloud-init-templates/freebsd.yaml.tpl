#cloud-config
hostname: {{ HOSTNAME }}
manage_etc_hosts: true

users:
  - name: {{ USERNAME }}
    groups: [wheel, operator]
    shell: /bin/sh
    sudo: ALL=(ALL) NOPASSWD:ALL
{{ PASSWORD_BLOCK }}
{{ SSH_KEYS_BLOCK }}

ssh_pwauth: {{ SSH_PWAUTH }}

timezone: {{ TIMEZONE }}

growpart:
  mode: auto
  devices: ['/']

chpasswd:
  expire: false

runcmd:
  - sysrc qemu_guest_agent_enable=YES
  - service qemu-guest-agent start

power_state:
  delay: "+1"
  mode: poweroff
  message: "cloud-init configuration complete"
  condition: true
