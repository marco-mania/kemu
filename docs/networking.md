# Networking

KEMU provides two network modes, both running entirely in userspace without root privileges, network bridges, or helper processes:

| Mode                | Backend                                  | VM-to-VM | DHCP              | NAT      | Port forwarding |
| ------------------- | ---------------------------------------- | -------- | ----------------- | -------- | --------------- |
| **User mode**       | QEMU's built-in SLIRP                    | No       | Yes (10.0.2.0/24) | Yes      | Yes             |
| **Virtual network** | In-process L2 switch + embedded libslirp | Yes      | Optional          | Optional | Yes             |

## User mode

Each VM uses an isolated connection via QEMU's SLIRP stack, with per-VM port forwarding rules exposing selected guest services to the host.

## Virtual networks

Configure virtual networks under *Settings → Virtual Networks* to connect multiple VMs to a private subnet. The entire network stack runs inside the KEMU process:

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

Each VM gets a switch port backed by one end of a `socketpair` passed to QEMU via file descriptor (`-netdev stream,addr.type=fd`). No socket file is created, preventing other processes from attaching. Ports accept frames only from their VM's configured MAC address to prevent spoofing; VMs with identical MAC addresses cannot share a virtual network.

The switch floods broadcast and multicast frames to connected VMs and forwards unknown unicast destinations only to VMs—never to the NAT uplink. Because no host kernel networking setup is required, virtual networks also work inside the Flatpak sandbox.

**Per-network options:**

- **DHCP:** Built-in DHCPv4 server; disable it for manual network configurations or cloud-init deployments with static addresses.
- **DNS:** Resolves local zone names and forwards external queries when NAT is enabled.
- **NAT:** Embedded `libslirp` terminates guest TCP/UDP connections and opens corresponding host connections alongside a DNS proxy. Without NAT, the network remains isolated, though the gateway still responds to ARP and ping to aid diagnostics of static configurations. NAT is IPv4-only and does not forward guest ping traffic.
- **Host access:** Disabled by default. When enabled alongside NAT, guests can reach services on the host's `127.0.0.1` via the gateway address.
- **Port forwarding:** Binds guest ports to `127.0.0.1` on the host using `proto/hostPort:guestIp:guestPort` (for example, `tcp/8080:192.168.100.50:80`).

When the last VM disconnects, KEMU stops the virtual network and closes its file descriptors.

**Exported VMs:** Generated start scripts link exported VMs from the same virtual network using QEMU multicast networking, providing Layer-2 connectivity without DHCP or internet access; configure static addresses manually or via cloud-init.

**Requirements:** QEMU ≥ 7.2 (for the `stream` network backend) and `libslirp` ≥ 4.7.

**Debugging:** Switch frames, DHCP exchanges, and NAT traffic are logged under the `org.kde.kemu` category (see *Troubleshooting* in the README).
