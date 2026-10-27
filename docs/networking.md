# Networking

KEMU offers two network modes. Both run entirely in userspace and need no root
privileges, no bridge and no helper process.

| Mode                | Backend                                  | VM-to-VM | DHCP              | NAT      | Port forwarding |
| ------------------- | ---------------------------------------- | -------- | ----------------- | -------- | --------------- |
| **User mode**       | QEMU's built-in SLIRP                    | No       | Yes (10.0.2.0/24) | Yes      | Yes             |
| **Virtual network** | In-process L2 switch + embedded libslirp | Yes      | Optional          | Optional | Yes             |

## User mode

Each VM gets its own isolated connection through QEMU's SLIRP stack. Port
forwarding rules per VM expose guest services on the host.

## Virtual networks

Virtual networks, defined under *Settings → Virtual Networks*, connect several
VMs to a private subnet. The whole stack runs inside the KEMU process:

```text
┌─── KEMU process ────────────────────────────────────────────────┐
│                                                                 │
│   VirtualSwitch (in-memory L2, one port per VM)                 │
│    ├── QEMU VM1  (-netdev stream, own socketpair)               │
│    ├── QEMU VM2  (-netdev stream, own socketpair)               │
│    ├── DhcpServer (optional, built-in)                          │
│    └── SlirpUplink (optional: NAT + DNS proxy)                  │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

Every VM gets its own switch port: one end of a socketpair that QEMU inherits
as a file descriptor (`-netdev stream,addr.type=fd`). Nothing is created in the
file system, so no other process can attach to a port. A port only accepts
frames from the VM's configured MAC address, so guests cannot impersonate each
other; VMs with the same MAC cannot join the same network. The switch floods
broadcast and multicast frames; frames for unknown destinations go to the VMs
only, never to the NAT uplink. Since nothing touches the host kernel, virtual
networks work unchanged in the Flatpak.

**Per network:**

- **DHCP:** A small DHCPv4 server built into KEMU assigns addresses. Disable it
  for static setups or cloud-init deployments with fixed addresses.
- **DNS:** Answers for the local zone, and forwards other names while NAT is on.
- **NAT:** An embedded libslirp terminates guest TCP/UDP connections and
  re-originates them on the host, including a DNS proxy. Without NAT the
  network is air-gapped; the gateway still answers ARP and ping so static
  setups remain diagnosable. NAT is IPv4 only and does not translate ping from
  guests.
- **Host access:** Off by default. With NAT, guests then reach services on the
  host's `127.0.0.1` through the gateway address.
- **Port forwarding:** Exposes ports of a guest on the host, bound to
  `127.0.0.1`. Format: `proto/hostPort:guestIp:guestPort`, e.g.
  `tcp/8080:192.168.100.50:80`.

When the last VM leaves a network, KEMU stops it and closes its descriptors;
nothing is left behind.

**Exported VMs:** Start scripts of exported VMs connect the VMs of the same
virtual network through QEMU multicast networking. This gives layer-2
connectivity between those VMs only – no DHCP and no internet access, so
configure static addresses (manually or with cloud-init).

**Requirements:** QEMU ≥ 7.2 (the `stream` network backend) and libslirp ≥ 4.7.

**Debugging:** Switch frames, DHCP exchanges and NAT traffic are logged under
the `org.kde.kemu` category; see *Troubleshooting* in the README.
