# Networking

KEMU provides two network modes. Both run entirely in userspace without
root privileges, network bridges or helper processes.

| Mode                | Backend                                  | VM-to-VM | DHCP              | NAT      | Port forwarding |
| ------------------- | ---------------------------------------- | -------- | ----------------- | -------- | --------------- |
| **User mode**       | QEMU's built-in SLIRP                    | No       | Yes (10.0.2.0/24) | Yes      | Yes             |
| **Virtual network** | In-process L2 switch + embedded libslirp | Yes      | Optional          | Optional | Yes             |

## User mode

Each VM uses an isolated connection through QEMU's SLIRP stack. Per-VM port
forwarding rules make selected guest services accessible from the host.

## Virtual networks

Configure virtual networks under *Settings → Virtual Networks* to connect
multiple VMs to a private subnet. The network stack runs entirely within the
KEMU process:

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

Each VM receives a switch port backed by one end of a socketpair, passed to
QEMU as a file descriptor (`-netdev stream,addr.type=fd`). No socket file is
created, so other processes cannot attach to the port. Each port accepts
frames only from its VM's configured MAC address, preventing guests from
impersonating one another; VMs with identical MAC addresses cannot share a
virtual network.

The switch floods broadcast and multicast frames to connected VMs. Frames
with unknown destinations are forwarded only to the VMs, never to the NAT
uplink. Because this does not require host kernel networking configuration,
virtual networks also work inside the Flatpak sandbox.

**Per network:**

- **DHCP:** A built-in DHCPv4 server assigns addresses. Disable it for
  manually configured networks or cloud-init deployments using static
  addresses.
- **DNS:** Resolves names in the local zone and forwards other queries when
  NAT is enabled.
- **NAT:** Embedded libslirp terminates guest TCP/UDP connections and opens
  corresponding connections on the host, with a DNS proxy. Without NAT, the
  network is isolated. The gateway still responds to ARP and ping, making
  static configurations easier to diagnose. NAT supports IPv4 only and does
  not forward guest ping traffic.
- **Host access:** Disabled by default. When enabled alongside NAT, guests
  can reach services on the host's `127.0.0.1` through the gateway address.
- **Port forwarding:** Exposes guest ports on the host, bound to
  `127.0.0.1`. Use `proto/hostPort:guestIp:guestPort`, for example
  `tcp/8080:192.168.100.50:80`.

When the last VM disconnects, KEMU stops the virtual network and closes its
file descriptors.

**Exported VMs:** Start scripts connect exported VMs belonging to the same
virtual network using QEMU multicast networking. This provides layer-2
connectivity between those VMs, but no DHCP or internet access. Configure
static addresses manually or through cloud-init.

**Requirements:** QEMU ≥ 7.2 (the `stream` network backend) and libslirp ≥ 4.7.

**Debugging:** Switch frames, DHCP exchanges and NAT traffic are logged in
the `org.kde.kemu` category. See *Troubleshooting* in the README.
