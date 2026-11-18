# KEMU Agent Protocol — Specification (Version 1, Draft)

Nov 14, 2026 · @Marco

## 1. Introduction

`kemu-agent` is a session agent for Linux guests running under KEMU. It shares the clipboard with the host and reports the guest display layout over a single virtio-serial port. KEMU speaks the protocol directly; QEMU merely transports the byte stream.

**Status:** Draft. Version 1 is not frozen; message numbers and field names may change until the first release of `kemu-agent`.

### 1.1 Goals

- Share all application-provided clipboard formats bidirectionally for both CLIPBOARD and PRIMARY selections.
- Report the actual guest display layout so the host can map the pointer accurately across all monitors.
- Run unprivileged in the guest as one user process per graphical session.
- Recover automatically from restarts on either side and from stale bytes on the port.
- Reserve protocol space for future display configuration and file transfer without breaking version 1.

### 1.2 Non-goals for version 1

- Windows and BSD guests (not precluded by the protocol, though version 1 targets Linux only).
- Drag-and-drop, file transfers exceeding the clipboard size limit, and display configuration (reserved in Section 9).
- Operations requiring root privileges—such as shutdown, clock synchronisation, filesystem freezing, or reading network addresses—which remain handled by `qemu-guest-agent`.
- Compatibility with the SPICE agent protocol; KEMU drops `spice-vdagent` and QEMU's `qemu-vdagent` chardev.

### 1.3 Why KEMU leaves the SPICE agent

- **Formats:** The VD agent protocol supports only a fixed set of clipboard types (UTF-8 text, PNG, BMP, TIFF, JPEG, and a file list requiring a SPICE WebDAV channel), while QEMU's clipboard core and D-Bus bridge carry UTF-8 text only.
- **Privileges:** `spice-vdagent` requires a root daemon (`spice-vdagentd`) to inject pointer events via `uinput`, alongside a session agent.
- **Layout:** The session agent reads the layout from XWayland, which applies a single uniform scale across all monitors, causing pointer offsets under per-monitor scaling.
- **Wayland:** Under KDE Plasma, an X11-only session agent observes clipboard changes only while an X11 window has focus.

### 1.4 Terminology

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in RFC 2119 and RFC 8174 when, and only when, they appear in capitals.

| Term      | Meaning                                                                                                 |
| --------- | ------------------------------------------------------------------------------------------------------- |
| Host      | The KEMU process running the virtual machine and owning the host end of the port.                       |
| Agent     | The `kemu-agent` process in one graphical session of the guest.                                         |
| Port      | The virtio-serial port named `org.kde.kemu.agent.0`.                                                    |
| Frame     | One header plus one message body on the port (Section 4).                                               |
| Session   | The state shared by host and agent from an accepted WELCOME until the next reset (Section 5).           |
| Selection | CLIPBOARD (copy and paste) or PRIMARY (select and middle-click).                                        |
| Offer     | An announcement that one side holds a selection, listing the formats it can deliver.                    |
| Owner     | The side whose latest offer for a selection is currently in force.                                      |
| Head      | The zero-based scanout index of the guest display device (matches QEMU's console `head` on virtio-gpu). |

### 1.5 Conventions

- Integers in the frame header are little-endian.
- Message bodies are CBOR (RFC 8949); examples use CBOR diagnostic notation.
- KiB means 1024 bytes; MiB means 1024 KiB.
- "G→H" denotes a message sent from agent to host, "H→G" from host to agent, and "both" either direction.

## 2. Architecture

The protocol runs between the host and one agent over a single virtio-serial port, with QEMU forwarding the byte stream without inspecting it. `qemu-guest-agent` runs alongside the agent on its own port and is outside this specification.

&#91;embedded content: kemu-agent architecture · host, QEMU, guest\]

The port carries clipboard data and display layout; pointer events still reach the guest through QEMU's tablet device, using the reported layout for host-side coordinate mapping.

| Component                  | Runs as                                | Role                                                                                                                    |
| -------------------------- | -------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| KEMU (host)                | Host user                              | Creates the port, owns its host end, speaks this protocol, and bridges it to the host clipboard and viewer pointer.     |
| QEMU                       | Host user                              | Provides `virtio-serial-pci` and a `virtserialport` backed by KEMU's chardev; reports open/close via `VSERPORT_CHANGE`. |
| `kemu-agent` (guest)       | Active graphical session user          | Opens the port, interacts with the compositor or X server for clipboard and layout, and speaks this protocol.           |
| `qemu-guest-agent` (guest) | Root                                   | Unchanged; uses its own port `org.qemu.guest_agent.0`.                                                                  |

### 2.1 Ports

| Port name                   | Status   | Purpose                                                 |
| --------------------------- | -------- | ------------------------------------------------------- |
| `org.kde.kemu.agent.0`      | defined  | Control port covering everything in this specification. |
| `org.kde.kemu.agent.bulk.0` | reserved | Bulk data such as file transfers (Section 9).           |
| `org.qemu.guest_agent.0`    | external | QEMU guest agent protocol (not covered here).           |

### 2.2 Privilege model

No part of this protocol runs as root in the guest. The agent runs as the session user, and a udev rule tags the port with `uaccess` so `logind` grants device access to the active session user on the seat (Section 11.1).

### 2.3 One agent at a time

Linux allows only one process to open a virtio-serial port at a time; a second `open()` fails with `EBUSY`. Consequently, only the active session's agent holds the port, and an agent MUST close the port when its session becomes inactive so the next active session can open it (Section 3.4).

### 2.4 What the host does with it

- **Clipboard:** Synchronises host and guest clipboards within the user-configured policy (Section 6).
- **Pointer:** Sends absolute pointer positions to a USB or virtio tablet, mapping coordinates into the guest's global coordinate space using the reported layout across multiple monitors (Sections 7 and 8).

## 3. Transport

While both ends are connected, the port acts as a reliable, ordered byte stream without message boundaries; bytes written while the peer is disconnected may arrive in a later session.

### 3.1 Guest side

- The agent opens `/dev/virtio-ports/org.kde.kemu.agent.0` with `O_RDWR | O_NONBLOCK | O_CLOEXEC`.
- While the host end is disconnected, `poll()` reports `POLLHUP`, writes fail with `EAGAIN`, and `read()` returns 0 once buffered data is drained.
- Once `POLLHUP` clears, the agent initiates the handshake (Section 5).

### 3.2 Host side

KEMU attaches the port alongside `qemu-guest-agent` on the same `virtio-serial-pci` controller:

```
-chardev socket,id=kemuagent,fd=<fd of a socketpair end>
-device virtserialport,id=kemuagent0,chardev=kemuagent,name=org.kde.kemu.agent.0
```

- The guest sees the host as connected for as long as KEMU keeps its socketpair end open.
- QEMU reports guest open and close transitions via QMP `VSERPORT_CHANGE` (`id` = `kemuagent0`, `open` = `true` or `false`).
- Neither a `qemu-vdagent` chardev nor a `com.redhat.spice.0` port is required.

### 3.3 Stale data

- While the guest has the port closed, QEMU stops reading from the chardev; any bytes written by the host remain in the socket buffer and are delivered to the next agent that opens the port.
- Partial frames left behind by a crashed agent arrive at the host ahead of the next agent's initial frame.
- The host SHOULD NOT write while the port is closed on the guest side. Both sides MUST handle stale bytes as specified in Section 5.3.

### 3.4 Session handover

- The agent MUST monitor the `Active` property of its `logind` session.
- When the session becomes inactive, the agent SHOULD send GOODBYE with reason `session-inactive` and MUST then close the port.
- When the session becomes active again, the agent reopens the port and runs the handshake.
- Because `logind` revokes device ACLs on session switch without invalidating already-open file descriptors, closing the port explicitly is the agent's responsibility.
- If `open()` fails with `EBUSY` or `EACCES`, the agent retries every 2 seconds while its session remains active.

### 3.5 Flow control

- Because virtio-serial guarantees lossless, in-order delivery while connected, the protocol omits checksums and retransmissions.
- Flow control relies on back-pressure: writers block when the peer stops reading.
- Both sides MUST use non-blocking I/O and MUST continue reading while output is pending to avoid deadlocks during simultaneous large transfers.

## 4. Framing and encoding

Each message consists of a 12-byte header followed by a CBOR body, allowing receivers to locate frame boundaries and skip unknown message types without decoding CBOR.

### 4.1 Frame header

```
offset  size  field   value
0       4     magic   0x4B 0x45 0x4D 0x55 ("KEMU")
4       2     type    message type, uint16 little-endian (Appendix A)
6       2     flags   uint16 little-endian, 0 in version 1
8       4     length  body length in bytes, uint32 little-endian
12      n     body    CBOR, exactly `length` bytes
```

- Senders MUST set `flags` to 0; receivers MUST ignore `flags` in version 1.
- `length` MUST NOT exceed 1 MiB (1,048,576 bytes); larger values constitute a framing error.

### 4.2 Body

- The body is a single CBOR data item representing a map (a zero-length body denotes an empty map).
- Map keys are text strings; receivers MUST ignore unknown keys, and senders MUST NOT repeat keys.
- Permitted CBOR items: unsigned and negative integers, byte strings, UTF-8 text strings, arrays, maps, `true`, `false`, `null`, and floats.
- Disallowed CBOR items: indefinite-length items, tags, `undefined`, and other simple values. Nesting of maps and arrays MUST NOT exceed 8 levels.
- Optional fields may be omitted; receivers treat `null` identically to an omitted field.
- Text strings MUST NOT exceed 4096 bytes unless specified otherwise for a field.

### 4.3 Field types

| Type     | CBOR                         | Notes                                                                                  |
| -------- | ---------------------------- | -------------------------------------------------------------------------------------- |
| `uint`   | unsigned integer             | Up to $2^{64} - 1$. Counters start at 1 per session and never wrap in practice.        |
| `int`    | unsigned or negative integer | Signed coordinates or timestamps.                                                      |
| `float`  | half, single, or double      | Receivers accept all three widths.                                                     |
| `bool`   | `true` or `false`            |                                                                                        |
| `tstr`   | text string                  | UTF-8.                                                                                 |
| `bstr`   | byte string                  | Raw bytes (never Base64).                                                              |
| `[T]`    | array of `T`                 |                                                                                        |
| `{…}`    | map                          | Contains the fields specified in the message table.                                    |

Dotted names such as `clipboard.to_host` refer to the `to_host` field inside the `clipboard` map.

### 4.4 Unknown message types

- A receiver MUST read and discard the body of any frame with an unrecognised type; this is not an error.
- A sender MUST NOT transmit messages belonging to a capability the peer did not advertise (Section 5.2). Core messages (Section 5) are always permitted.
- Types `0x8000`–`0xFFFF` are reserved for experimentation and MUST NOT appear in released software.

### 4.5 Malformed input

- Framing errors comprise: invalid magic, `length` exceeding 1 MiB, a body that is not well-formed CBOR or not a map, and a partial frame receiving no further bytes for 2 seconds.
- Upon a framing error, the receiver discards its input buffer and resynchronises (Section 5.3).
- A well-formed frame with a missing or mistyped required field is not a framing error; the receiver SHOULD reply with ERROR `bad-message` and otherwise ignore the frame.

## 5. Connection lifecycle

The agent always initiates a session with HELLO, to which the host responds with WELCOME followed by POLICY. A random nonce in HELLO—echoed in WELCOME—distinguishes the new session from stale bytes.

### 5.1 Handshake

1. The agent opens the port and waits until the host end connects (Section 3.1).
2. The agent generates a fresh 16-byte random nonce, sends HELLO, and enters HANDSHAKE.
3. Upon receiving a valid HELLO, the host resets its session state, selects the protocol version, sends WELCOME followed by POLICY, and enters READY.
4. Upon receiving a WELCOME matching its current nonce, the agent enters READY, sends DISPLAY\_LAYOUT if negotiated, and—once POLICY arrives—sends a CLIPBOARD\_OFFER for each held selection permitted by policy.
5. If no matching WELCOME arrives, the agent resends HELLO with a new nonce after 2 seconds, doubling the backoff interval up to 30 seconds.

**HELLO** (0x0001, G→H)

| Field          | Type                                                 | Req. | Meaning                                                                                        |
| -------------- | ---------------------------------------------------- | ---- | ---------------------------------------------------------------------------------------------- |
| `nonce`        | `bstr`, 16 bytes                                     | yes  | Fresh random value generated for every HELLO.                                                  |
| `versions`     | `[uint]`                                             | yes  | Supported protocol versions, e.g., `[1]`.                                                      |
| `agent`        | `{name: tstr, version: tstr}`                        | yes  | Agent identifier, e.g., `{"name": "kemu-agent", "version": "0.2.0"}`.                          |
| `capabilities` | `[tstr]`                                             | yes  | Advertised capabilities (Appendix B).                                                          |
| `os`           | `{id: tstr, version_id: tstr, pretty_name: tstr}`    | no   | Values copied from `/etc/os-release`.                                                          |
| `session`      | `{type: tstr, desktop: tstr}`                        | no   | `type` is `wayland` or `x11`; `desktop` is `XDG_CURRENT_DESKTOP`.                              |
| `backends`     | `{clipboard: tstr}`                                  | no   | `ext-data-control`, `wlr-data-control`, `x11`, or `none` (helps explain unavailable features). |
| `limits`       | `{max_transfer: uint}`                               | no   | Maximum clipboard item size in bytes accepted by the agent (default 64 MiB).                   |

**WELCOME** (0x0002, H→G)

| Field          | Type                          | Req. | Meaning                                                                     |
| -------------- | ----------------------------- | ---- | --------------------------------------------------------------------------- |
| `nonce`        | `bstr`                        | yes  | Echoes the `nonce` from the corresponding HELLO.                            |
| `version`      | `uint`                        | yes  | Selected protocol version (highest version supported by both sides).        |
| `host`         | `{name: tstr, version: tstr}` | yes  | Host identifier, e.g., `{"name": "KEMU", "version": "1.4.0"}`.              |
| `capabilities` | `[tstr]`                      | yes  | Advertised capabilities (Appendix B).                                       |
| `limits`       | `{max_transfer: uint}`        | no   | Maximum clipboard item size in bytes accepted by the host (default 64 MiB). |

If no common version exists, the host sends ERROR `unsupported-version` (with `versions` listing its supported versions and `nonce` echoing HELLO) instead of WELCOME and continues waiting for HELLO. The agent SHOULD log the mismatch and wait at least 5 minutes before sending another HELLO.

### 5.2 Versions and capabilities

- The version is a single integer; incompatible changes require a new version.
- Backwards-compatible extensions use a new capability or optional field.
- A capability is active only when advertised by both sides, and negotiated capabilities remain fixed for the session.

### 5.3 Resynchronisation

In scan mode, a receiver searches incoming bytes for the frame magic and reads the subsequent header, accepting only handshake-valid frame types with `length` ≤ 4096 (HELLO, WELCOME, and ERROR MUST therefore never exceed 4096 bytes):

- **Agent:** WELCOME, ERROR, and HELLO\_REQUEST.
- **Host:** HELLO.

For any other candidate, the receiver advances one byte past the start of the magic and continues scanning without waiting for a body. The agent accepts a WELCOME or ERROR only if it carries the agent's current nonce; HELLO\_REQUEST carries no nonce and is always accepted.

Because each direction is FIFO, stale bytes always precede the first frame of the new session. Since the host writes WELCOME only after reading HELLO, all data following the accepted WELCOME belongs to the current session. If the host accepts a stale HELLO from a terminated agent, the new agent ignores the mismatched WELCOME and resets the host with its own HELLO (Section 5.9).

**HELLO\_REQUEST** (0x0003, H→G) has an empty body. The agent responds in any state with a new HELLO and fresh nonce (rate-limited to at most once per second), resetting its session state first if currently READY.

### 5.4 Session state

A session reset clears clipboard ownership, serials, pending clipboard requests, and the layout generation. The host retains the last reported display layout for pointer mapping until a new one arrives (Section 8).

### 5.5 POLICY

**POLICY** (0x0010, H→G) is sent immediately after WELCOME and whenever the user changes a policy setting.

| Field                   | Type   | Req. | Meaning                                                                                        |
| ----------------------- | ------ | ---- | ---------------------------------------------------------------------------------------------- |
| `clipboard.to_host`     | `bool` | yes  | Host accepts clipboard offers from the guest.                                                  |
| `clipboard.to_guest`    | `bool` | yes  | Host offers its clipboard to the guest.                                                        |
| `clipboard.primary`     | `bool` | yes  | PRIMARY selection is shared in the allowed directions.                                         |
| `clipboard.files`       | `bool` | no   | Files are shared in the allowed directions (Section 6.8; default `false`).                     |
| `clipboard.files_limit` | `uint` | no   | Maximum combined byte size of files in a single offer (default 0).                             |

- Before the first POLICY arrives, all fields default to `false`.
- The agent MUST NOT send offers disallowed by policy, and the host MUST ignore any disallowed offers.
- When a direction becomes enabled, the side currently holding the selection SHOULD offer it immediately so the user need not copy again.
- When `to_host` becomes `false`, the host drops guest offers; when `to_guest` becomes `false`, the host sends CLIPBOARD\_RELEASE for its active offers.

### 5.6 PING and PONG

**PING** (0x0004, both) and **PONG** (0x0005, both) carry a single `id` (`uint`) field; PONG echoes the `id` of the PING it answers.

- Receivers MUST respond to PING promptly.
- The host SHOULD send a PING after 10 seconds without incoming traffic and mark the agent as unresponsive in the UI if no PONG arrives within another 10 seconds, while keeping the session open.

### 5.7 GOODBYE

**GOODBYE** (0x0006, both) signals session termination.

| Field     | Type   | Req. | Meaning                                                              |
| --------- | ------ | ---- | -------------------------------------------------------------------- |
| `reason`  | `tstr` | yes  | `session-inactive`, `shutdown`, `restart`, `disabled`, or `error`.   |
| `message` | `tstr` | no   | Optional diagnostic text for logs.                                   |

- After sending GOODBYE, the sender transmits nothing further in that session.
- An agent closes the port immediately after sending GOODBYE.
- When the host sends GOODBYE (e.g., because the user disabled integration), the agent resets to IDLE and waits for HELLO\_REQUEST or a new host connection; it MUST NOT send HELLO before then.

### 5.8 ERROR

**ERROR** (0x0007, both) reports a non-fatal error without terminating the session.

| Field      | Type     | Req. | Meaning                                                          |
| ---------- | -------- | ---- | ---------------------------------------------------------------- |
| `code`     | `tstr`   | yes  | Error code (Appendix C).                                         |
| `message`  | `tstr`   | no   | Optional diagnostic text for logs.                               |
| `type`     | `uint`   | no   | Message type that triggered the error.                           |
| `id`       | `uint`   | no   | Request ID that triggered the error.                             |
| `nonce`    | `bstr`   | no   | With `unsupported-version`: `nonce` of the rejected HELLO.       |
| `versions` | `[uint]` | no   | With `unsupported-version`: protocol versions supported by host. |

### 5.9 State machines

**Agent:**

| State           | Event                                      | Action                                             | Next state |
| --------------- | ------------------------------------------ | -------------------------------------------------- | ---------- |
| CLOSED          | Session active, `open()` succeeds          | —                                                  | WAITING    |
| WAITING         | `POLLHUP` clears                           | Send HELLO                                         | HANDSHAKE  |
| HANDSHAKE       | WELCOME with current nonce                 | Send DISPLAY\_LAYOUT and offers                    | READY      |
| HANDSHAKE       | Timeout or HELLO\_REQUEST                  | Send new HELLO                                     | HANDSHAKE  |
| HANDSHAKE       | ERROR `unsupported-version`                | Log; send new HELLO after 5 minutes at earliest    | HANDSHAKE  |
| READY           | Framing error or HELLO\_REQUEST            | Reset, send new HELLO                              | HANDSHAKE  |
| READY           | GOODBYE from host                          | Reset                                              | IDLE       |
| IDLE            | HELLO\_REQUEST                             | Send HELLO                                         | HANDSHAKE  |
| any open state  | `POLLHUP`                                  | Reset                                              | WAITING    |
| any open state  | Session inactive                           | Send GOODBYE, close port                           | CLOSED     |

**Host:**

| State       | Event                                           | Action                       | Next state  |
| ----------- | ----------------------------------------------- | ---------------------------- | ----------- |
| WAIT\_HELLO | Valid HELLO                                     | Send WELCOME, POLICY         | READY       |
| WAIT\_HELLO | No HELLO 1 second after `VSERPORT_CHANGE` open  | Send HELLO\_REQUEST          | WAIT\_HELLO |
| READY       | HELLO                                           | Reset, send WELCOME, POLICY  | READY       |
| READY       | Framing error                                   | Reset, send HELLO\_REQUEST   | WAIT\_HELLO |
| READY       | GOODBYE from agent                              | Reset                        | WAIT\_HELLO |
| any         | `VSERPORT_CHANGE` closed                        | Reset, stop writing          | WAIT\_HELLO |

## 6. Clipboard

Each side advertises clipboard contents via an offer containing MIME types without payload data; the peer fetches a format only when requested. Monotonic serials order offers and resolve simultaneous copies deterministically in favour of the host.

Capabilities: `clipboard` (for CLIPBOARD) and `clipboard.primary` (for PRIMARY).

### 6.1 Selections

| Value | Selection | Requirements                                                    |
| ----- | --------- | --------------------------------------------------------------- |
| 0     | CLIPBOARD | Capability `clipboard`                                          |
| 1     | PRIMARY   | Capability `clipboard.primary` and POLICY `clipboard.primary`   |

All other values are reserved; receivers ignore clipboard messages for unknown selections.

### 6.2 Ownership and serials

For each selection, both sides track three state variables:

- `local_serial`: serial of the last sent offer (0 if none).
- `peer_serial`: serial of the last accepted peer offer (0 if none).
- `owner`: `local`, `peer`, or `none`.

**Sending an offer:**

1. Increment `local_serial`.
2. Send CLIPBOARD\_OFFER with `serial` = `local_serial` and `seen` = `peer_serial`.
3. Set `owner` = `local`.

**Receiving an offer with serial `s` and `seen` = `x`:**

1. If `s` ≤ `peer_serial`, ignore the stale offer.
2. If `owner` = `local` and `x` < `local_serial`, the peer sent its offer before seeing ours. In this conflict, the host wins:
   - The host ignores the agent's offer and sets `peer_serial` = `s`.
   - The agent accepts the host's offer as in step 3.
3. Otherwise accept the offer: set `peer_serial` = `s`, set `owner` = `peer`, and install a local clipboard proxy for the offered types.

If a side rejects an offer due to local policy (such as the host's password filter), it still updates `peer_serial` = `s`; if `owner` was `peer`, it sets `owner` = `none` and removes its proxy because the peer no longer holds the previous content.

**Local changes:**

- When a local application claims the selection, the side sends an offer if policy allows it and at least one type is shareable (Section 6.6).
- If the new content cannot be shared and `owner` = `local`, it sends CLIPBOARD\_RELEASE instead.
- When the local selection is cleared while `owner` = `local`, it sends CLIPBOARD\_RELEASE and sets `owner` = `none`.
- Installing a local proxy modifies the local clipboard; a side MUST NOT respond to its own proxy update with a new offer.

In a simultaneous-copy conflict, the host's content replaces the guest's on both sides.

### 6.3 CLIPBOARD\_OFFER

**CLIPBOARD\_OFFER** (0x0100, both)

| Field       | Type                        | Req. | Meaning                                                                                                |
| ----------- | --------------------------- | ---- | ------------------------------------------------------------------------------------------------------ |
| `selection` | `uint`                      | yes  | Selection ID (Section 6.1).                                                                            |
| `serial`    | `uint`                      | yes  | Strictly increasing per sender and selection, starting at 1 per session.                               |
| `seen`      | `uint`                      | yes  | Last peer serial accepted by the sender (0 if none).                                                   |
| `types`     | `[tstr]`                    | yes  | 1 to 64 MIME types in preference order (may be empty if `files` is present).                           |
| `sizes`     | `{tstr: uint}`              | no   | Known byte sizes for specific MIME types (e.g., prefetched text).                                      |
| `files`     | `{count: uint, size: uint}` | no   | Present when the selection holds shared files (Section 6.8): entry count and total file size in bytes. |

### 6.4 CLIPBOARD\_RELEASE

**CLIPBOARD\_RELEASE** (0x0101, both) withdraws a previous offer; the sender sets `owner` = `none`.

| Field       | Type   | Req. | Meaning                        |
| ----------- | ------ | ---- | ------------------------------ |
| `selection` | `uint` | yes  | Selection ID (Section 6.1).    |
| `serial`    | `uint` | yes  | Serial of the withdrawn offer. |

The receiver acts only if `owner` = `peer` and `peer_serial` = `serial`, setting `owner` = `none` and removing its proxy if still active.

### 6.5 Fetching data

The requester sends CLIPBOARD\_REQUEST to the owner, which responds with one or more CLIPBOARD\_DATA frames or a single CLIPBOARD\_FAIL.

**CLIPBOARD\_REQUEST** (0x0102, both)

| Field       | Type   | Req. | Meaning                                                            |
| ----------- | ------ | ---- | ------------------------------------------------------------------ |
| `id`        | `uint` | yes  | Monotonically increasing request ID, unique per requester/session. |
| `selection` | `uint` | yes  | Selection ID (Section 6.1).                                        |
| `serial`    | `uint` | yes  | Serial of the target offer.                                        |
| `type`      | `tstr` | yes  | One of the MIME types listed in the offer.                         |

**CLIPBOARD\_DATA** (0x0103, both)

| Field  | Type   | Req. | Meaning                                                             |
| ------ | ------ | ---- | ------------------------------------------------------------------- |
| `id`   | `uint` | yes  | Matching request ID.                                                |
| `data` | `bstr` | yes  | Next data chunk, at most 256 KiB (may be empty).                    |
| `size` | `uint` | no   | Total payload size in bytes (included on the first chunk if known). |
| `last` | `bool` | no   | `true` on the final chunk (default `false`).                        |

**CLIPBOARD\_FAIL** (0x0104, both)

| Field     | Type   | Req. | Meaning                                                                               |
| --------- | ------ | ---- | ------------------------------------------------------------------------------------- |
| `id`      | `uint` | yes  | Matching request ID.                                                                  |
| `reason`  | `tstr` | yes  | `stale`, `not-offered`, `too-large`, `unavailable`, `denied`, or `busy` (Appendix C). |
| `message` | `tstr` | no   | Optional diagnostic text for logs.                                                    |

**CLIPBOARD\_CANCEL** (0x0105, both) carries a single `id` (`uint`) field to abort an in-flight request.

**Transfer rules:**

- Chunks arrive in order and concatenate to form the complete payload.
- The total payload MUST NOT exceed the receiver's `limits.max_transfer`; otherwise, the owner aborts with CLIPBOARD\_FAIL `too-large` (even mid-transfer), and the requester discards partial data.
- Chunks SHOULD be 64 KiB.
- With multiple concurrent transfers, the sender SHOULD interleave chunks across requests and MUST allow other queued control messages (such as PING or DISPLAY\_LAYOUT) to pass between chunks.
- A requester may keep at most 8 requests open simultaneously; the owner MAY reject additional requests with `busy`.
- Upon receiving CLIPBOARD\_CANCEL, the owner MUST stop sending and emit no further response; the requester ignores any trailing DATA or FAIL frames for that ID.
- The requester SHOULD cancel any request that receives no data for 10 seconds.
- Requests MAY be issued proactively at any time while an offer is current (used by the agent to prefetch small text before the source application exits).
- After a new offer supersedes an old one, the owner MAY still serve requests for the previous serial if cached, or respond with `stale`.

### 6.6 MIME types

- Types MUST be printable ASCII MIME names up to 255 bytes, normally containing `/`.
- Non-MIME X11 targets (`TARGETS`, `MULTIPLE`, `TIMESTAMP`, `SAVE_TARGETS`, `UTF8_STRING`, `STRING`, `TEXT`) MUST NOT be sent.
- **Text:** If any text format is available, the offer MUST include `text/plain;charset=utf-8` with valid UTF-8 data; additional text types MAY also be offered.
- **Images:** If any image format is available, the offer SHOULD include `image/png` (converted on demand if necessary).
- Private implementation types prefixed with `application/x-kemu-` (such as echo-prevention markers) MUST NOT be offered.
- File transfers follow Section 6.8; senders MUST NOT offer local file-operation types (`x-special/gnome-copied-files`, `application/x-kde-cutselection`, `application/vnd.portal.files`, `application/vnd.portal.filetransfer`).
- When serving `text/uri-list`, the owner MUST strip `file:` URIs and respond with `unavailable` if none remain.
- `x-kde-passwordManagerHint` is the sole permitted type without `/` and passes through like any other type so receivers (such as KEMU's password filter) can enforce their own policy.

### 6.7 Rate limits

- Because PRIMARY updates continuously during text selection, a sender MUST NOT emit more than one PRIMARY offer per 250 ms and SHOULD send the final state once the selection settles for 250 ms.
- Because some applications update CLIPBOARD twice in rapid succession, a sender SHOULD debounce CLIPBOARD changes by 50 ms before sending an offer.

### 6.8 Files

Requires capabilities `clipboard.files` and `clipboard`. Files and directories copied on one side are listed and served on demand by the owner, staged into a local directory by the receiver, and exposed on the receiver's clipboard as `file:` URIs.

File sharing is active only when POLICY `clipboard.files` is `true`, in the policy-permitted directions, and when the total selection fits within `clipboard.files_limit`.

#### 6.8.1 Offering files

When a selection's `text/uri-list` contains `file:` URIs, the owner builds a file listing:

- Includes one entry per regular file and directory recursively.
- Omits symbolic links, device nodes, sockets, and FIFOs without following links.
- Assigns each entry a relative path rooted at the copied top-level names, deduplicating colliding top-level names (e.g., `notes (2).txt`).

**Path validation rules (enforced by both owner and requester):**

- Components are `/` separated; no component may be empty, `.`, `..`, contain `/` or NUL, or exceed 255 bytes.
- Full paths must be valid UTF-8, at most 1024 bytes, and contain at most 32 components.
- Parent directories precede their descendants, and all paths are unique.

The owner includes `files` in CLIPBOARD\_OFFER (Section 6.3) only if:

- Both sides advertised `clipboard.files` and POLICY permits files in that direction.
- All paths satisfy the path validation rules.
- The listing contains at most 10,000 entries and total file bytes do not exceed `clipboard.files_limit`.

Otherwise, the offer is sent without `files`. Offers with `files` MAY have an empty `types` list; `text/uri-list` continues to strip `file:` URIs (Section 6.6). Cut operations are treated identically to copy operations, leaving source files untouched.

#### 6.8.2 Messages

**CLIPBOARD\_FILE\_LIST** (0x0110, both) requests a slice of the file listing.

| Field       | Type   | Req. | Meaning                                                          |
| ----------- | ------ | ---- | ---------------------------------------------------------------- |
| `id`        | `uint` | yes  | Request ID (from the same sequence as CLIPBOARD\_REQUEST).       |
| `selection` | `uint` | yes  | Selection ID (Section 6.1).                                      |
| `serial`    | `uint` | yes  | Serial of the target offer.                                      |
| `start`     | `uint` | no   | Zero-based index of the first requested entry (default 0).       |

**CLIPBOARD\_FILE\_ENTRIES** (0x0111, both) returns listing entries.

| Field     | Type    | Req. | Meaning                                                                   |
| --------- | ------- | ---- | ------------------------------------------------------------------------- |
| `id`      | `uint`  | yes  | Matching request ID.                                                      |
| `total`   | `uint`  | yes  | Total number of entries in the listing.                                   |
| `entries` | `[map]` | yes  | Up to 256 entries starting at `start` (empty only if `start` ≥ `total`).  |

**Entry map fields:**

| Field   | Type   | Req.       | Meaning                                                                |
| ------- | ------ | ---------- | ---------------------------------------------------------------------- |
| `path`  | `tstr` | yes        | Relative path (Section 6.8.1).                                         |
| `type`  | `tstr` | yes        | `file` or `dir` (entries with any other type MUST be ignored).         |
| `size`  | `uint` | for `file` | File size in bytes.                                                    |
| `mtime` | `int`  | no         | Modification time in seconds since 1970-01-01 UTC.                     |

Entry indices are zero-based positions in the listing, which remains immutable while the offer is active.

**CLIPBOARD\_FILE\_READ** (0x0112, both) reads a byte range from a file.

| Field       | Type   | Req. | Meaning                                             |
| ----------- | ------ | ---- | --------------------------------------------------- |
| `id`        | `uint` | yes  | Request ID.                                         |
| `selection` | `uint` | yes  | Selection ID (Section 6.1).                         |
| `serial`    | `uint` | yes  | Serial of the target offer.                         |
| `file`      | `uint` | yes  | Zero-based index of an entry with `type` = `file`.  |
| `offset`    | `uint` | yes  | Byte offset within the file.                        |
| `length`    | `uint` | yes  | Requested byte count (`1` to `262,144`).            |

**CLIPBOARD\_FILE\_DATA** (0x0113, both) returns file bytes.

| Field  | Type   | Req. | Meaning                                                                                        |
| ------ | ------ | ---- | ---------------------------------------------------------------------------------------------- |
| `id`   | `uint` | yes  | Matching request ID.                                                                           |
| `data` | `bstr` | yes  | Bytes starting at `offset` (up to `length`; shorter only at EOF, empty at or past EOF).        |

**File transfer rules:**

- Each request receives exactly one response (ENTRIES, DATA, or CLIPBOARD\_FAIL); CLIPBOARD\_CANCEL and the 8-request concurrency limit (Section 6.5) apply.
- Read requests are stateless and may arrive in any order (allowing future revisions to fetch files lazily on paste without protocol changes).
- The owner responds with `stale` if the serial is outdated, `denied` if policy disallows files, `not-offered` if the offer lacks `files` or `file` does not index a file entry, `unavailable` if the file cannot be read, and `changed` if the file's size or `mtime` no longer matches the listing.
- File transfers are governed by `clipboard.files_limit` rather than `limits.max_transfer`.
- Like CLIPBOARD\_DATA, file responses MUST yield to queued control messages (Section 6.5).

#### 6.8.3 Receiving files

Upon receiving a policy-permitted offer with `files`, the receiver:

1. Verifies that `files.size` fits within `clipboard.files_limit` and available free space in the staging directory, aborting immediately if not.
2. Fetches the complete listing and validates every path, directory ordering, entry count, and cumulative size against `files.size` (any violation aborts the transfer).
3. Creates the directory hierarchy and reads all files sequentially into a dedicated staging directory for that offer.
4. Publishes the offer on its local clipboard once all files complete, including fetched MIME types, `text/uri-list` with `file:` URIs for top-level entries, and any desktop-specific formats (such as `x-special/gnome-copied-files`).

**Staging rules:**

- **All-or-nothing:** If any file fails (including `changed`), the receiver deletes all staged files for that offer and publishes the clipboard offer without files.
- A file must never exceed its listed `size`, and a premature EOF aborts the transfer.
- Any superseding offer, CLIPBOARD\_RELEASE, or local copy cancels active file requests and deletes staged files.
- The staging directory is restricted to the user (`mode 0700`); staged files are created non-executable without preserving source ownership or permissions (`mtime` MAY be preserved).
- Staged files remain for at least 10 minutes after being superseded by a newer offer so in-progress pastes can finish, and are purged completely on agent startup and exit.
- URIs pointing into the local staging directory are never re-offered as files (Section 11.6).

#### 6.8.4 Example

The guest user copies a directory containing two files; the host permits file sharing up to 256 MiB:

```
H→G POLICY {"clipboard": {"to_host": true, "to_guest": true, "primary": false,
                           "files": true, "files_limit": 268435456}}
G→H CLIPBOARD_OFFER {"selection": 0, "serial": 7, "seen": 2,
                     "types": ["text/plain;charset=utf-8"],
                     "files": {"count": 3, "size": 1300000}}
H→G CLIPBOARD_FILE_LIST {"id": 31, "selection": 0, "serial": 7}
G→H CLIPBOARD_FILE_ENTRIES {"id": 31, "total": 3, "entries": [
        {"path": "report", "type": "dir"},
        {"path": "report/draft.odt", "type": "file", "size": 1250000, "mtime": 1794720000},
        {"path": "report/notes.txt", "type": "file", "size": 50000, "mtime": 1794720300}]}
H→G CLIPBOARD_FILE_READ {"id": 32, "selection": 0, "serial": 7, "file": 1, "offset": 0, "length": 262144}
H→G CLIPBOARD_FILE_READ {"id": 33, "selection": 0, "serial": 7, "file": 1, "offset": 262144, "length": 262144}
G→H CLIPBOARD_FILE_DATA {"id": 32, "data": h'…'}          (262144 bytes)
G→H CLIPBOARD_FILE_DATA {"id": 33, "data": h'…'}
...
```

Once `report/notes.txt` finishes transferring, the host clipboard exposes `text/uri-list` containing `file:///…/clipboard/7/report`.

## 7. Display layout

The agent reports each monitor's geometry in the guest's global coordinate space alongside the bounding rectangle mapped by absolute pointer devices, allowing the host to position the pointer accurately across monitors.

Capability: `display.layout`.

### 7.1 DISPLAY\_LAYOUT

**DISPLAY\_LAYOUT** (0x0200, G→H) is sent after the handshake and whenever the layout changes (debounced by 100 ms).

| Field        | Type                                          | Req. | Meaning                                                                                      |
| ------------ | --------------------------------------------- | ---- | -------------------------------------------------------------------------------------------- |
| `generation` | `uint`                                        | yes  | Monotonically increasing counter per session.                                                |
| `space`      | `{x: int, y: int, width: uint, height: uint}` | yes  | Global-coordinate rectangle onto which the compositor maps absolute pointer devices.         |
| `outputs`    | `[output]`                                    | yes  | All outputs (enabled or disabled), up to 16 entries.                                         |
| `source`     | `tstr`                                        | no   | Diagnostic backend identifier: `kwin`, `wlroots`, `mutter`, `xdg-output`, or `x11-randr`.    |

**Each `output` map:**

| Field       | Type                                         | Req.       | Meaning                                                                                                    |
| ----------- | -------------------------------------------- | ---------- | ---------------------------------------------------------------------------------------------------------- |
| `name`      | `tstr`                                       | yes        | Connector name, e.g., `Virtual-1`.                                                                         |
| `head`      | `uint`                                       | no         | Display device scanout index (Section 7.3; omitted if unknown).                                            |
| `enabled`   | `bool`                                       | yes        | Whether the output is active.                                                                              |
| `x`, `y`    | `int`                                        | if enabled | Top-left corner in global coordinates.                                                                     |
| `width`, `height` | `uint`                                 | if enabled | Logical dimensions in global coordinates after scaling and rotation.                                       |
| `mode`      | `{width: uint, height: uint, refresh: uint}` | no         | Current pixel mode (`refresh` in millihertz).                                                              |
| `scale`     | `float`                                      | no         | Display scale factor (default `1.0`).                                                                      |
| `transform` | `tstr`                                       | no         | `normal`, `90`, `180`, `270`, `flipped`, `flipped-90`, `flipped-180`, or `flipped-270` (default `normal`). |
| `primary`   | `bool`                                       | no         | Whether this is the primary output (default `false`).                                                      |

### 7.2 Global coordinates

- **Wayland:** Logical compositor coordinates as reported by `xdg-output`.
- **X11:** Root window pixel coordinates.
- **`space`:** KWin maps absolute pointer devices to the bounding rectangle of all outputs, whereas X11 maps them to the root window. If unknown, the agent reports the bounding rectangle of enabled outputs.

### 7.3 Heads

- The Linux `virtio-gpu` driver creates one `Virtual` DRM connector per scanout in scanout order.
- Because connector numbering increments per type across all guest DRM devices, `Virtual-1` is not guaranteed to be head 0.
- The agent collects all connectors belonging to the `virtio-gpu` device, sorts them numerically, and assigns heads `0, 1, 2, …` in order (Section 11.4).
- Compositors name outputs after the DRM connector (e.g., `Virtual-2`), linking each output to its head; if uncertain, the agent MUST omit `head`.
- The host associates each head with its D-Bus display console via the console's `Head` property.

### 7.4 DISPLAY\_LAYOUT\_QUERY

**DISPLAY\_LAYOUT\_QUERY** (0x0201, H→G) has an empty body; the agent responds with a DISPLAY\_LAYOUT carrying an incremented `generation`, even if unchanged.

## 8. Pointer mapping on the host (non-normative)

Using a single tablet device and the reported layout, the host maps pointer coordinates on any monitor into global tablet coordinates without sending pointer events over the agent port or requiring `uinput` or root in the guest.

### 8.1 From viewer position to tablet value

For a pointer at framebuffer pixel `(px, py)` on head `h`:

1. Locate the enabled output with `head` = `h` (falling back to KEMU's configured or heuristic layout if absent).
2. Invert the output's `transform` so `(px, py)` aligns with the guest's logical orientation (no-op for `normal`).
3. Map to global coordinates: $gx = x + px \times \text{width} / \text{mode.width}$ and $gy = y + py \times \text{height} / \text{mode.height}$ (substituting framebuffer dimensions if `mode` is omitted).
4. Normalise to tablet range ($0\dots32767$, rounded down): $tx = (gx - \text{space.x}) \times 32768 / \text{space.width}$ and $ty = (gy - \text{space.y}) \times 32768 / \text{space.height}$.

Step 4 inverts QEMU's tablet scaling, where `0`–`0x7FFF` (32,768 steps) spans `space`.

### 8.2 Delivery to QEMU

| Path                                           | Resolution                                   | Notes                                                                                                                                             |
| ---------------------------------------------- | -------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| QMP `input-send-event` (`abs` events)          | Full: 32,768 steps across `space`            | Accepts raw `0`–`0x7FFF` values; coalesce motion events to the viewer frame rate to limit QMP traffic.                                            |
| D-Bus `SetAbsPosition` on primary console      | Primary monitor pixel width                  | QEMU scales by the primary console size and rejects out-of-bounds coordinates (two equal side-by-side monitors yield 2-pixel steps; used today).  |

Button and scroll events SHOULD use the same transport path as preceding motion events so clicks cannot overtake cursor movement.

### 8.3 Lifetime of the layout

- The host retains the last layout across a new HELLO since physical monitor layout does not change across protocol handshakes.
- When the guest closes the port, the host discards the layout (as the session ended and a login screen or subsequent session may use a different arrangement) and reverts to heuristic detection until a new DISPLAY\_LAYOUT arrives.
- Single-monitor setups require no coordinate mapping.

## 9. Reserved extensions

Version 1 reserves message ranges and capability names for two planned extensions; implementations MUST NOT use them until specified in a future revision.

| Range             | Allocation                        |
| ----------------- | --------------------------------- |
| `0x0001`–`0x00FF` | Core messages (Section 5)         |
| `0x0100`–`0x01FF` | Clipboard (Section 6)             |
| `0x0200`–`0x02FF` | Display layout (Section 7)        |
| `0x0300`–`0x03FF` | Reserved: display configuration   |
| `0x0400`–`0x04FF` | Reserved: file transfer           |
| `0x0500`–`0x7FFF` | Unassigned                        |
| `0x8000`–`0xFFFF` | Experimental (never in releases)  |

### 9.1 Display configuration

Capability `display.configure`: allows the host to request mode, position, scale, or primary-output changes via the compositor's native interface (KWin output management, `wlr-output-management`, Mutter's `org.gnome.Mutter.DisplayConfig`, or X11 RandR), returning the status and updated DISPLAY\_LAYOUT. Open questions include conflict resolution with guest-initiated changes and persistence across guest reboots.

### 9.2 File transfer

Capability `file.transfer` (using data port `org.kde.kemu.agent.bulk.0`): enables unlimited file/folder transfers and drag-and-drop in both directions without blocking control traffic, supporting progress reporting, cancellation, host confirmation for guest-originated transfers, path traversal protection, symlink exclusion, and zero-copy path rewriting inside `virtiofs` shared folders.

## 10. Security considerations

The host treats all guest messages as untrusted input. While the agent trusts the host (which controls the VM), version 1 restricts host access strictly to clipboard and display layout.

### 10.1 Host

- Validates all size and count limits before allocating memory (frame length, CBOR nesting depth, string lengths, max 64 MIME types, max 16 outputs).
- Treats `size` fields as hints only and enforces `max_transfer` against actual received bytes.
- Enforces user-configured transfer directions per VM regardless of agent behaviour, MAY strip arbitrary MIME types before publishing proxies, and MAY coalesce rapid guest offers.
- Ignores layout dimensions above 32,768 or more than 16 outputs (a spoofed layout can at worst misplace pointer clicks inside the VM).

### 10.2 Guest

- Port access is restricted via `uaccess` to the active seat session user (Sections 2.2 and 3.4); other local users cannot access the port.
- **Limitation:** Because an already-open file descriptor persists until closed, a malicious process in an inactive session could retain the port and continue receiving host offers. Multi-user guests with mutually untrusted users are outside version 1's threat model.
- The agent runs unprivileged as the session user, executes no commands, writes no files (aside from clipboard file staging), and routes incoming data solely to its own session clipboard.

### 10.3 Secrets and logs

- Entries marked with `x-kde-passwordManagerHint` = `secret` propagate that hint across both sides so KEMU's password filter can block them in both directions (enabled by default).
- Neither side logs clipboard payloads (logs may record MIME types and byte sizes), and HELLO omits the guest username.

### 10.4 Files

- File sharing is disabled by default and controlled independently from clipboard sharing.
- Receivers stage files into a dedicated `0700` directory, validate all relative paths, never follow symlinks, strip executable permissions, and check free disk space as well as entry/size caps beforehand.
- Owners serve files strictly by listing index for the active offer, preventing arbitrary path reads.

## 11. Implementation notes: Linux guest

The Linux guest package installs one user binary, one udev rule, one systemd user unit, and one XDG autostart file, requiring Qt 6 (Core, Gui, DBus) and `libwayland-client` without KDE libraries.

### 11.1 Port access

`/usr/lib/udev/rules.d/70-kemu-agent.rules` (must sort before `73-seat-late.rules` so `uaccess` is applied):

```
SUBSYSTEM=="virtio-ports", ATTR{name}=="org.kde.kemu.agent.0", TAG+="uaccess"
```

### 11.2 Start-up

`/usr/lib/systemd/user/kemu-agent.service`:

```
[Unit]
Description=KEMU guest session agent
PartOf=graphical-session.target
After=graphical-session.target
ConditionPathExists=/dev/virtio-ports/org.kde.kemu.agent.0

[Service]
ExecStart=/usr/bin/kemu-agent
Restart=on-failure
RestartSec=2

[Install]
WantedBy=graphical-session.target
```

- An XDG autostart entry with `X-systemd-skip=true` launches the agent in desktop sessions without systemd user units.
- The agent acquires the D-Bus session name `org.kde.kemu.Agent` (exiting if already owned) and exits cleanly with status `0` if `/dev/virtio-ports/org.kde.kemu.agent.0` is absent.

### 11.3 Clipboard backends

| Environment                                       | Backend                                         | Notes                                                                                                               |
| ------------------------------------------------- | ----------------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| KDE Plasma (Wayland)                              | `ext-data-control`, fallback `wlr-data-control` | KWin exposes data-control to unsandboxed clients; older versions (e.g., Debian 13) provide `wlr-data-control` only. |
| wlroots compositors (Sway, Hyprland, labwc)       | `ext-data-control`, fallback `wlr-data-control` |                                                                                                                     |
| GNOME (Wayland)                                   | X11 via Xwayland (`QT_QPA_PLATFORM=xcb`)        | Mutter lacks data-control and bridges X11/Wayland selections without focus (same approach as `spice-vdagent`).      |
| X11 desktops (Xfce, Cinnamon, MATE, Plasma X11)   | `QClipboard` on `xcb`                           | XFixes reports selection changes without window focus.                                                              |

The agent selects its backend at startup before Qt initialises a platform plugin (overriding any session-wide `QT_QPA_PLATFORM=wayland`), speaks `data-control` over its own Wayland connection, and reports the chosen backend in HELLO `backends.clipboard`.

### 11.4 Heads

1. Locate the DRM card whose `/sys/class/drm/cardN/device/driver` resolves to `virtio_gpu`.
2. Enumerate its `/sys/class/drm/cardN-Virtual-M` connectors and sort them numerically by `M`.
3. Assign zero-based head index `k` to the `k`-th connector (omitting `head` if there is not exactly one `virtio-gpu` card).

### 11.5 Layout sources

| Environment              | Source                                                      | `space`                          |
| ------------------------ | ----------------------------------------------------------- | -------------------------------- |
| Wayland (KWin, wlroots)  | `wl_output` name with `xdg-output` logical position/size    | Bounding rectangle of outputs    |
| GNOME (Wayland)          | Same as above, via a dedicated Wayland connection           | Bounding rectangle of outputs    |
| X11                      | RandR outputs and CRTC geometry                             | Root window                      |

On GNOME Wayland, the agent MUST NOT read the display layout from XWayland's RandR, which reports uniform scaling across all monitors.

### 11.6 Echoes and prefetch

- Every proxy installed by the agent carries the private MIME type `application/x-kemu-agent-proxy` to prevent re-offering echoed clipboard changes.
- Immediately after a local copy, the agent prefetches text formats up to 64 KiB and advertises their sizes in `sizes`.
- Host files are staged under `$XDG_CACHE_HOME/kemu-agent/clipboard/<pid>/`, published via `text/uri-list` and `x-special/gnome-copied-files`, and excluded from outgoing file offers.

### 11.7 Testing

A pipe-connected host simulator enables automated CI testing of `kemu-agent` under headless compositors such as `sway` or `kwin_wayland --virtual`.

## 12. Implementation notes: KEMU host

KEMU replaces the `qemu-vdagent` chardev and D-Bus clipboard bridge with a dedicated port client modelled on `GuestAgentClient`, while feeding reported layouts into `MultiheadMouseMapping`.

### 12.1 What changes

| Area                                                      | Previous implementation                                                                        | With `kemu-agent`                                                                                |
| --------------------------------------------------------- | ---------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| QEMU command line (`qemucommandbuilder.cpp`)              | `qemu-vdagent` chardev (`clipboard=on`, `mouse=on` for multi-monitor) and `com.redhat.spice.0` | Socket chardev and `org.kde.kemu.agent.0` port (Section 3.2)                                     |
| Clipboard (`QemuClipboardManager`, `Display1.Clipboard`)  | UTF-8 text only; guest grabs without serial dropped by QEMU                                    | Protocol client supporting all MIME types, PRIMARY, and bidirectional password filtering         |
| Multi-monitor pointer (`MultiheadMouseMapping`)           | Heuristic layout from cursor feedback; `spice-vdagent` pointer positioning                     | Exact layout from DISPLAY\_LAYOUT (with heuristic detection retained as fallback)                |
| QEMU capability check (`settingsdialog.cpp`)              | Required `qemu-vdagent` chardev                                                                | Checks for `virtio-serial-pci` instead                                                           |
| Guest hint (`editvmdialog.cpp`)                           | Recommended `spice-vdagent` for multiple monitors                                              | Recommends `kemu-agent` for Linux guests                                                         |
| Viewer status bar                                         | —                                                                                              | Clipboard indicator shows `kemu-agent` connection status and version from HELLO                  |

### 12.2 Port client

- Owns one end of a Unix socketpair passed to QEMU and tracks guest open/close state via `VSERPORT_CHANGE` for `kemuagent0`.
- Implements incremental framing with scan-mode resynchronisation (Section 5.3) and a 2-second partial-frame timeout (Section 4.5).
- Decodes CBOR with `QCborStreamReader` (rejecting indefinite-length items) and encodes with `QCborMap`.
- Paces outgoing clipboard chunks so no new chunk is queued while ≥ 128 KiB awaits socket write, bounding control-message latency.

### 12.3 Clipboard on the host

- Updates POLICY from *Devices → Clipboard* and enforces the password filter on both host and guest offers (Section 6.2).
- Prefetches guest offers eagerly up to 32 MiB before updating the host clipboard because `QMimeData` serves pasting applications synchronously; larger payloads remain on the guest.
- Snapshots host clipboard data upon offer within the same 32 MiB budget and deduplicates identical content (as Wayland re-announces selections on every focus change).
- Shares PRIMARY only when enabled by the user and supported by the host session.
- Shares files only when *Share Files* is enabled (default off) up to the configured limit (default 256 MiB), staging guest files in a per-viewer cache directory removed when the viewer closes.

### 12.4 Removing SPICE support

- Removes the `qemu-vdagent` configuration block, capability check, `QemuClipboardManager` (with tests), and `MultiheadMouseMapping::setGuestAgentPointer`.
- Existing VMs require no manual changes and receive the new port automatically on next boot; guests running only `spice-vdagent` lose clipboard sharing until `kemu-agent` is installed.
- KEMU MAY display a notification if `kemu-agent` has not connected within 60 seconds after `qemu-guest-agent` connects.

## Appendix A. Message types

| Type     | Name                     | Direction | Capability        | Section |
| -------- | ------------------------ | --------- | ----------------- | ------- |
| `0x0001` | HELLO                    | G→H       | core              | 5.1     |
| `0x0002` | WELCOME                  | H→G       | core              | 5.1     |
| `0x0003` | HELLO\_REQUEST           | H→G       | core              | 5.3     |
| `0x0004` | PING                     | both      | core              | 5.6     |
| `0x0005` | PONG                     | both      | core              | 5.6     |
| `0x0006` | GOODBYE                  | both      | core              | 5.7     |
| `0x0007` | ERROR                    | both      | core              | 5.8     |
| `0x0010` | POLICY                   | H→G       | core              | 5.5     |
| `0x0100` | CLIPBOARD\_OFFER         | both      | `clipboard`       | 6.3     |
| `0x0101` | CLIPBOARD\_RELEASE       | both      | `clipboard`       | 6.4     |
| `0x0102` | CLIPBOARD\_REQUEST       | both      | `clipboard`       | 6.5     |
| `0x0103` | CLIPBOARD\_DATA          | both      | `clipboard`       | 6.5     |
| `0x0104` | CLIPBOARD\_FAIL          | both      | `clipboard`       | 6.5     |
| `0x0105` | CLIPBOARD\_CANCEL        | both      | `clipboard`       | 6.5     |
| `0x0110` | CLIPBOARD\_FILE\_LIST    | both      | `clipboard.files` | 6.8     |
| `0x0111` | CLIPBOARD\_FILE\_ENTRIES | both      | `clipboard.files` | 6.8     |
| `0x0112` | CLIPBOARD\_FILE\_READ    | both      | `clipboard.files` | 6.8     |
| `0x0113` | CLIPBOARD\_FILE\_DATA    | both      | `clipboard.files` | 6.8     |
| `0x0200` | DISPLAY\_LAYOUT          | G→H       | `display.layout`  | 7.1     |
| `0x0201` | DISPLAY\_LAYOUT\_QUERY   | H→G       | `display.layout`  | 7.4     |

## Appendix B. Capabilities

| Capability          | Status   | Meaning                                                 |
| ------------------- | -------- | ------------------------------------------------------- |
| `clipboard`         | defined  | CLIPBOARD selection sharing (Section 6).                |
| `clipboard.primary` | defined  | PRIMARY selection sharing; requires `clipboard`.        |
| `clipboard.files`   | defined  | File sharing over clipboard (Section 6.8); needs `clipboard`. |
| `display.layout`    | defined  | Display layout reporting (Section 7).                   |
| `display.configure` | reserved | Display configuration (Section 9.1).                    |
| `file.transfer`     | reserved | Dedicated bulk file transfer (Section 9.2).             |

## Appendix C. Error codes

| Code                  | Used in         | Meaning                                                                  |
| --------------------- | --------------- | ------------------------------------------------------------------------ |
| `unsupported-version` | ERROR           | No mutually supported protocol version (carries `versions` and `nonce`). |
| `bad-message`         | ERROR           | Required field is missing or has an invalid type.                        |
| `internal`            | ERROR           | Unrecoverable internal sender error.                                     |
| `stale`               | CLIPBOARD\_FAIL | Offer serial is no longer current and cached data is unavailable.        |
| `not-offered`         | CLIPBOARD\_FAIL | Requested MIME type or file index was not part of the offer.             |
| `too-large`           | CLIPBOARD\_FAIL | Payload exceeds the requester's `limits.max_transfer`.                   |
| `unavailable`         | CLIPBOARD\_FAIL | Source application failed to supply data or no shareable data remains.   |
| `denied`              | CLIPBOARD\_FAIL | Transfer disallowed by the owner's active policy.                        |
| `busy`                | CLIPBOARD\_FAIL | Concurrent open request limit exceeded.                                  |
| `changed`             | CLIPBOARD\_FAIL | Target file size or `mtime` changed since the listing was built.         |

## Appendix D. Examples

Bodies are shown in CBOR diagnostic notation; `h'…'` denotes a byte string.

### D.1 One frame, byte by byte

PING with `id` = 7:

```
4B 45 4D 55   magic "KEMU"
04 00         type 0x0004 (PING)
00 00         flags
05 00 00 00   length 5
A1            map, 1 pair
62 69 64      text "id"
07            unsigned 7
```

### D.2 Handshake

```
G→H HELLO {"nonce": h'3f9c2e71a05b4d88c1e6f0729ab3d415', "versions": [1],
           "agent": {"name": "kemu-agent", "version": "0.2.0"},
           "capabilities": ["clipboard", "clipboard.primary", "display.layout"],
           "session": {"type": "wayland", "desktop": "KDE"},
           "backends": {"clipboard": "ext-data-control"}}
H→G WELCOME {"nonce": h'3f9c2e71a05b4d88c1e6f0729ab3d415', "version": 1,
             "host": {"name": "KEMU", "version": "1.4.0"},
             "capabilities": ["clipboard", "display.layout"]}
H→G POLICY {"clipboard": {"to_host": true, "to_guest": true, "primary": false}}
G→H DISPLAY_LAYOUT {… as in D.5 …}
```

Because the host omits `clipboard.primary`, PRIMARY is not shared during this session.

### D.3 Copy in the guest, paste on the host

```
G→H CLIPBOARD_OFFER {"selection": 0, "serial": 1, "seen": 0,
                     "types": ["text/html", "text/plain;charset=utf-8"],
                     "sizes": {"text/plain;charset=utf-8": 11}}
H→G CLIPBOARD_REQUEST {"id": 1, "selection": 0, "serial": 1,
                       "type": "text/plain;charset=utf-8"}
G→H CLIPBOARD_DATA {"id": 1, "data": h'48656c6c6f20776f726c64', "size": 11, "last": true}
```

### D.4 Both sides copy at the same moment

Initial state: host owns CLIPBOARD (`local_serial` = 5, `peer_serial` = 2); agent has accepted host serial 5 (`peer_serial` = 5, `local_serial` = 2).

```
H→G CLIPBOARD_OFFER {"selection": 0, "serial": 6, "seen": 2, "types": […]}
G→H CLIPBOARD_OFFER {"selection": 0, "serial": 3, "seen": 5, "types": […]}
```

- **Host:** Owns the selection and sees `seen` (5) < `local_serial` (6). Host wins the conflict, ignores the offer, and sets `peer_serial` = 3.
- **Agent:** Owns the selection and sees `seen` (2) < `local_serial` (3). Agent yields, accepts the host's offer, and sets `peer_serial` = 6.
- Both sides end up holding the host's clipboard content.

### D.5 Two monitors with different scales

```
G→H DISPLAY_LAYOUT {"generation": 1,
  "space": {"x": 0, "y": 0, "width": 3200, "height": 1080},
  "outputs": [
    {"name": "Virtual-1", "head": 0, "enabled": true, "x": 0, "y": 0,
     "width": 1920, "height": 1080,
     "mode": {"width": 1920, "height": 1080, "refresh": 60000},
     "scale": 1.0, "primary": true},
    {"name": "Virtual-2", "head": 1, "enabled": true, "x": 1920, "y": 0,
     "width": 1280, "height": 720,
     "mode": {"width": 2560, "height": 1440, "refresh": 60000},
     "scale": 2.0}],
  "source": "kwin"}
```

For a pointer at framebuffer pixel `(1280, 720)` on head 1 (per Section 8.1):

- $gx = 1920 + 1280 \times 1280 / 2560 = 2560$ and $gy = 720 \times 720 / 1440 = 360$.
- $tx = 2560 \times 32768 / 3200 = 26214$ and $ty = 360 \times 32768 / 1080 = 10922$ (rounded down).

## References

- [RFC 8949: Concise Binary Object Representation (CBOR)](https://www.rfc-editor.org/rfc/rfc8949)
- [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119) and [RFC 8174](https://www.rfc-editor.org/rfc/rfc8174): requirement levels
- [ext-data-control-v1](https://wayland.app/protocols/ext-data-control-v1): Wayland clipboard protocol for privileged clients
- Checked in upstream sources (November 2026): Linux `drivers/char/virtio_console.c` (single open, `POLLHUP`, blocking writes), `drivers/gpu/drm/virtio/virtgpu_display.c` (one connector per scanout), and `drivers/gpu/drm/drm_connector.c` (per-type connector numbering); QEMU `hw/char/virtio-console.c` and `hw/char/virtio-serial-bus.c` (open/close events, paused reads while closed), `qapi/ui.json` (`input-send-event` range `0`–`0x7FFF`), `ui/dbus-console.c` (`SetAbsPosition` scaling), and `ui/dbus-display1.xml` (console `Head` property); KWin `src/backends/libinput/connection.cpp` (absolute pointer mapping onto workspace) and `src/backends/drm/drm_connector.cpp` (output naming).
