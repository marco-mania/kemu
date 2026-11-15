# KEMU Agent Protocol — Specification (Version 1, Draft)

Draft of 2026-11-14.

## 1. Introduction

kemu-agent is a session agent for Linux guests of KEMU. It shares the clipboard with the host and reports the guest display layout over one virtio-serial port. KEMU speaks the protocol directly; QEMU only carries the bytes.

Status: draft. Version 1 is not frozen. Message numbers and field names may change until the first release of kemu-agent.

### 1.1 Goals

- Share every clipboard format an application offers, in both directions, for the CLIPBOARD and PRIMARY selections.
- Tell the host the real guest display layout, so that it can place the pointer on any monitor.
- Run without root in the guest: one user process per graphical session.
- Recover without user action from restarts on either side and from stale bytes on the port.
- Leave room for display configuration and file transfer without breaking version 1.

### 1.2 Non-goals for version 1

- Windows and BSD guests. The protocol does not rule them out, but version 1 implementations target Linux only.
- File transfer and display configuration. Section 9 reserves them.
- Anything that needs root, such as shutdown, setting the clock, freezing file systems or reading network addresses. qemu-guest-agent stays responsible for these, and KEMU keeps supporting it in full.
- Compatibility with the SPICE agent protocol. KEMU drops spice-vdagent and QEMU's `qemu-vdagent` chardev.

### 1.3 Why KEMU leaves the SPICE agent

- Formats: the VD agent protocol knows a fixed list of clipboard types (UTF-8 text, PNG, BMP, TIFF, JPEG and a file list that needs a SPICE WebDAV channel). QEMU's clipboard core and its D-Bus bridge carry UTF-8 text only.
- Privileges: spice-vdagent needs a root daemon (spice-vdagentd) that injects the pointer through uinput, plus a session agent that talks to it.
- Layout: the session agent reads the layout from XWayland, which scales all monitors alike. With per-monitor scaling the pointer lands in the wrong place.
- Wayland: under KDE Plasma, an X11-only session agent sees clipboard changes only while an X11 window has focus.

### 1.4 Terminology

The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as described in RFC 2119 and RFC 8174 when, and only when, they appear in capitals.

| Term | Meaning |
| --- | --- |
| Host | The KEMU process that runs the virtual machine and owns the host end of the port. |
| Agent | The kemu-agent process in one graphical session of the guest. |
| Port | The virtio-serial port named `org.kde.kemu.agent.0`. |
| Frame | One header plus one message body on the port (Section 4). |
| Session | The state shared by host and agent from an accepted WELCOME until the next reset (Section 5). |
| Selection | CLIPBOARD (copy and paste) or PRIMARY (select and middle-click). |
| Offer | An announcement that one side holds a selection, with the list of formats it can deliver. |
| Owner | The side whose latest offer for a selection is in force. |
| Head | The zero-based scanout index of the guest display device. For virtio-gpu it matches QEMU's console `head` property. |

### 1.5 Conventions

- Integers in the frame header are little-endian.
- Message bodies are CBOR (RFC 8949). Examples use CBOR diagnostic notation.
- KiB means 1024 bytes and MiB means 1024 KiB.
- "G→H" marks a message the agent sends to the host, "H→G" the reverse, "both" either direction.

## 2. Architecture

The protocol runs between the host and one agent, over one virtio-serial port. QEMU carries the bytes without reading them. qemu-guest-agent runs beside the agent on its own port and is not part of this protocol.

```
 Host: KEMU                       QEMU                              Guest (Linux)

 Guest agent client   <-socket->  virtserialport          <-virtio->  qemu-guest-agent
 shutdown, time, IPs              org.qemu.guest_agent.0              root, system service

 Agent port client *  <-socket->  virtserialport *        <-virtio->  kemu-agent *
 clipboard, layout                org.kde.kemu.agent.0                user, active session
        |                                                                   ^
        | layout                                                            | clipboard, layout
        v                                                                   v
 Viewer               --D-Bus-->  USB or virtio tablet    --input-->  Compositor
 maps pointer by layout           absolute, 0 to 0x7FFF               Wayland or X11

 * new in this specification; the qemu-guest-agent path stays as it is
```

The new port carries clipboard and layout. The pointer still reaches the guest through QEMU's tablet; only its mapping on the host uses the layout.

| Component | Runs as | Role |
| --- | --- | --- |
| KEMU (host) | host user | Creates the port, owns its host end, speaks this protocol, connects it to the host clipboard and the viewer's pointer handling. |
| QEMU | host user | Provides `virtio-serial-pci` and a `virtserialport` backed by a chardev whose other end KEMU owns. Reports guest open and close through the QMP event `VSERPORT_CHANGE`. |
| kemu-agent (guest) | the user of the active graphical session | Opens the port, talks to the compositor or X server for clipboard and layout, speaks this protocol. |
| qemu-guest-agent (guest) | root | Unchanged. Uses its own port `org.qemu.guest_agent.0`. |

### 2.1 Ports

| Port name | Status | Purpose |
| --- | --- | --- |
| `org.kde.kemu.agent.0` | defined | Control port: everything in this specification. |
| `org.kde.kemu.agent.bulk.0` | reserved | Bulk data such as file transfer (Section 9). |
| `org.qemu.guest_agent.0` | external | QEMU guest agent protocol, not covered here. |

### 2.2 Privilege model

No part of this protocol runs as root in the guest. The agent runs as the session user. A udev rule tags the port with `uaccess`, so logind grants access to the user of the active session on the seat (Section 11.1).

### 2.3 One agent at a time

Linux lets only one process open a virtio-serial port at a time; a second `open()` fails with `EBUSY`. So only the agent of the active session holds the port. An agent MUST close the port when its session becomes inactive, so that the agent of the next active session can open it (Section 3.4).

### 2.4 What the host does with it

- Clipboard: the host keeps the host clipboard and the guest clipboard in step, within the policy the user set (Section 6).
- Pointer: the host sends absolute pointer positions to a USB or virtio tablet. With several monitors it maps each position into the guest's global coordinates, using the layout the agent reports (Sections 7 and 8).

## 3. Transport

The port is a reliable, ordered byte stream while both ends are connected. It has no message boundaries, and bytes written while the other end is away can arrive in a later session.

### 3.1 Guest side

- The agent opens `/dev/virtio-ports/org.kde.kemu.agent.0` with `O_RDWR | O_NONBLOCK | O_CLOEXEC`.
- Linux exposes whether the host end is connected. While it is not, `poll()` reports `POLLHUP`, a write would block (`EAGAIN`), and `read()` returns 0 once no data is left.
- When `POLLHUP` clears, the agent starts the handshake (Section 5).

### 3.2 Host side

KEMU adds the port next to the existing one for qemu-guest-agent, on the same `virtio-serial-pci` controller:

```
-chardev socket,id=kemuagent,fd=<fd of a socketpair end>
-device virtserialport,id=kemuagent0,chardev=kemuagent,name=org.kde.kemu.agent.0
```

- The guest sees the host as connected while the chardev is connected. With a socketpair, that is as long as KEMU keeps its end open.
- QEMU reports the guest opening and closing the port as `VSERPORT_CHANGE` with `id` = `kemuagent0` and `open` = true or false.
- The VM needs no `qemu-vdagent` chardev and no `com.redhat.spice.0` port.

### 3.3 Stale data

- While the guest has the port closed, QEMU does not read from the chardev. Bytes the host writes stay in the socket buffer and reach the next agent that opens the port.
- Bytes from an agent that dies in the middle of a frame reach the host ahead of the next agent's first frame.
- The host SHOULD NOT write while the port is closed on the guest side. Both sides MUST still handle stale bytes as described in Section 5.3.

### 3.4 Session handover

- The agent MUST watch the `Active` property of its own logind session.
- When the session becomes inactive, the agent SHOULD send GOODBYE with reason `session-inactive` and MUST then close the port.
- When the session becomes active again, the agent reopens the port and runs the handshake.
- logind removes the device ACL when the session changes, but that does not revoke a file descriptor that is already open. Closing the port is therefore the agent's duty.
- If `open()` fails with `EBUSY` or `EACCES`, the agent retries every 2 seconds while its session is active.

### 3.5 Flow control

- virtio-serial does not lose or reorder bytes while both ends are connected. The protocol therefore has no checksums or retransmission.
- Flow control is back-pressure: a writer blocks while the reader does not read.
- Both sides MUST use non-blocking I/O and MUST keep reading while output is pending. Otherwise two sides sending large data at the same time can deadlock.

## 4. Framing and encoding

Every message is one frame: a 12-byte header followed by a CBOR body. The header lets a receiver find frame boundaries and skip unknown types without decoding CBOR.

### 4.1 Frame header

```
offset  size  field   value
0       4     magic   0x4B 0x45 0x4D 0x55 ("KEMU")
4       2     type    message type, uint16 little-endian (Appendix A)
6       2     flags   uint16 little-endian, 0 in version 1
8       4     length  body length in bytes, uint32 little-endian
12      n     body    CBOR, exactly `length` bytes
```

- Senders MUST set `flags` to 0. Receivers MUST ignore `flags` in version 1.
- `length` MUST NOT exceed 1 MiB (1 048 576). A larger value is a framing error.

### 4.2 Body

- The body is exactly one CBOR data item, and it is a map. A zero-length body means an empty map.
- Map keys are text strings. Receivers MUST ignore keys they do not know. Senders MUST NOT repeat a key.
- Allowed items: unsigned and negative integers, byte strings, UTF-8 text strings, arrays, maps, `true`, `false`, `null` and floats.
- Not allowed: indefinite-length items, tags, `undefined` and other simple values. Maps and arrays nest at most 8 levels deep.
- An optional field may be absent. Receivers treat `null` like an absent field.
- Text strings are at most 4096 bytes unless a field says otherwise.

### 4.3 Field types

The message tables in later sections use these types.

| Type | CBOR | Notes |
| --- | --- | --- |
| `uint` | unsigned integer | Up to 2^64 − 1. Counters start at 1 in each session and never wrap in practice. |
| `int` | unsigned or negative integer | Coordinates, which can be negative. |
| `float` | half, single or double float | Receivers accept all three widths. |
| `bool` | `true` or `false` |  |
| `tstr` | text string | UTF-8. |
| `bstr` | byte string | Raw data, never Base64. |
| `[T]` | array of T |  |
| `{…}` | map | Fields as listed in the table. |

A dotted field name such as `clipboard.to_host` means the field `to_host` inside the map `clipboard`.

### 4.4 Unknown message types

- A receiver MUST read and discard the body of a frame whose type it does not know. This is not an error.
- A sender MUST NOT send messages of a capability the peer did not announce (Section 5.2). The core messages in Section 5 are always allowed.
- Types 0x8000 to 0xFFFF are for experiments and MUST NOT appear in released software.

### 4.5 Malformed input

- These are framing errors: wrong magic, `length` above the limit, a body that is not well-formed CBOR or not a map, and a partial frame that gets no further byte for 2 seconds.
- On a framing error the receiver discards its input buffer and resynchronises (Section 5.3).
- A well-formed frame with a missing or mistyped required field is not a framing error. The receiver SHOULD answer with ERROR `bad-message` and otherwise ignore the frame.

## 5. Connection lifecycle

The agent always starts a session with HELLO; the host answers with WELCOME and then POLICY. A random nonce in HELLO, echoed in WELCOME, separates the new session from stale bytes.

### 5.1 Handshake

1. The agent opens the port and waits until the host end is connected (Section 3.1).
2. The agent draws a fresh 16-byte random nonce, sends HELLO and enters HANDSHAKE.
3. The host, on a valid HELLO, resets its session state, chooses the version, and sends WELCOME followed by POLICY. The host is now READY.
4. The agent, on a WELCOME that carries its current nonce, is READY. It then sends DISPLAY\_LAYOUT if negotiated. Once POLICY has arrived, it sends a CLIPBOARD\_OFFER for each selection it holds and the policy allows.
5. Without a matching WELCOME, the agent sends a new HELLO with a new nonce after 2 seconds, then doubles the wait up to 30 seconds.

**HELLO** (0x0001, G→H)

| Field | Type | Req. | Meaning |
| --- | --- | --- | --- |
| `nonce` | `bstr`, 16 bytes | yes | Random, fresh for every HELLO. |
| `versions` | `[uint]` | yes | Protocol versions the agent supports, for example `[1]`. |
| `agent` | `{name: tstr, version: tstr}` | yes | For example `{"name": "kemu-agent", "version": "0.2.0"}`. |
| `capabilities` | `[tstr]` | yes | Appendix B. |
| `os` | `{id: tstr, version_id: tstr, pretty_name: tstr}` | no | Copied from `/etc/os-release`. |
| `session` | `{type: tstr, desktop: tstr}` | no | `type` is `wayland` or `x11`; `desktop` is `XDG_CURRENT_DESKTOP`. |
| `backends` | `{clipboard: tstr}` | no | `ext-data-control`, `wlr-data-control`, `x11` or `none`. Lets the host explain a missing feature. |
| `limits` | `{max_transfer: uint}` | no | Largest clipboard item the agent accepts, in bytes. Default 64 MiB. |

**WELCOME** (0x0002, H→G)

| Field | Type | Req. | Meaning |
| --- | --- | --- | --- |
| `nonce` | `bstr` | yes | The nonce of the HELLO it answers. |
| `version` | `uint` | yes | The chosen version, the highest one both support. |
| `host` | `{name: tstr, version: tstr}` | yes | For example `{"name": "KEMU", "version": "1.4.0"}`. |
| `capabilities` | `[tstr]` | yes | Appendix B. |
| `limits` | `{max_transfer: uint}` | no | Largest clipboard item the host accepts. Default 64 MiB. |

Without a common version, the host sends ERROR `unsupported-version` instead of WELCOME. It carries the fields `versions`, listing the host's own, and `nonce`, echoing the HELLO. The host keeps waiting for HELLO. The agent SHOULD log the mismatch and wait at least 5 minutes before the next HELLO.

### 5.2 Versions and capabilities

- The version is a single number. Incompatible changes get a new version.
- Compatible additions get a new capability or a new optional field.
- A feature is in use only when both sides list its capability. Capabilities stay fixed for the session.

### 5.3 Resynchronisation

In scan mode a receiver searches the input for the magic and reads the header behind it. It considers only the frames it expects during the handshake:

- the agent: WELCOME, ERROR and HELLO\_REQUEST;
- the host: HELLO;
- in both cases only with `length` at most 4096. HELLO, WELCOME and ERROR MUST therefore stay within 4096 bytes.

For any other candidate it moves one byte past the start of the magic and keeps searching, without waiting for the body. The agent accepts a WELCOME only if it carries the agent's current nonce.

The agent accepts a WELCOME or an ERROR only if it carries the agent's current nonce. HELLO\_REQUEST carries no nonce and is always accepted.

This is safe because each direction is FIFO. Stale bytes always arrive ahead of the first frame of the new session. The host writes WELCOME only after it has read the HELLO, so everything after the accepted WELCOME belongs to the current session.

The host may accept a stale HELLO from an agent that died right after sending it. The new agent ignores the WELCOME, because the nonce differs, and its next HELLO resets the host (Section 5.9).

**HELLO\_REQUEST** (0x0003, H→G) has an empty body. The agent answers in any state with a new HELLO and a new nonce, at most once per second. A READY agent first resets its session state.

### 5.4 Session state

A reset on either side drops: clipboard ownership and serials, outstanding clipboard requests, and the layout generation. The host keeps the last reported layout for pointer mapping until a new one arrives (Section 8).

### 5.5 POLICY

**POLICY** (0x0010, H→G) is sent right after WELCOME and again whenever the user changes a setting.

| Field | Type | Req. | Meaning |
| --- | --- | --- | --- |
| `clipboard.to_host` | `bool` | yes | The host accepts offers from the guest. |
| `clipboard.to_guest` | `bool` | yes | The host offers its own clipboard to the guest. |
| `clipboard.primary` | `bool` | yes | PRIMARY is shared too, in the directions allowed above. |

- Until the first POLICY, every field counts as false.
- The agent MUST NOT send offers that the policy does not allow. The host MUST ignore such offers anyway.
- When a direction becomes allowed, the side that holds the selection SHOULD offer it at once, so the user does not have to copy again.
- When `to_host` becomes false, the host drops the guest's offers. When `to_guest` becomes false, the host sends CLIPBOARD\_RELEASE for its own offers.

### 5.6 PING and PONG

**PING** (0x0004, both) and **PONG** (0x0005, both) carry one field, `id` (`uint`). A PONG echoes the `id` of the PING it answers.

- The receiver of a PING MUST answer promptly.
- The host SHOULD send a PING after 10 seconds without input. Without a PONG within 10 more seconds it marks the agent as unresponsive in its interface, but keeps the session.

### 5.7 GOODBYE

**GOODBYE** (0x0006, both) announces the end of the session.

| Field | Type | Req. | Meaning |
| --- | --- | --- | --- |
| `reason` | `tstr` | yes | `session-inactive`, `shutdown`, `restart`, `disabled` or `error`. |
| `message` | `tstr` | no | Text for logs. |

- After GOODBYE the sender sends nothing more in this session.
- An agent closes the port after its GOODBYE.
- A GOODBYE from the host means the host does not want an agent right now, for example because the user switched the integration off. The agent resets and waits for HELLO\_REQUEST or a new host connection. It MUST NOT send HELLO before that.

### 5.8 ERROR

**ERROR** (0x0007, both) reports a problem without ending the session.

| Field | Type | Req. | Meaning |
| --- | --- | --- | --- |
| `code` | `tstr` | yes | Appendix C. |
| `message` | `tstr` | no | Text for logs. |
| `type` | `uint` | no | The message type the error refers to. |
| `id` | `uint` | no | The request the error refers to. |
| `nonce` | `bstr` | no | With `unsupported-version`: the nonce of the HELLO it answers. |
| `versions` | `[uint]` | no | With `unsupported-version`: the versions the host supports. |

### 5.9 State machines

Agent:

| State | Event | Action | Next state |
| --- | --- | --- | --- |
| CLOSED | Session active, `open()` succeeds | — | WAITING |
| WAITING | `POLLHUP` clears | Send HELLO | HANDSHAKE |
| HANDSHAKE | WELCOME with current nonce | Send DISPLAY\_LAYOUT and offers | READY |
| HANDSHAKE | No WELCOME in time, or HELLO\_REQUEST | Send new HELLO | HANDSHAKE |
| HANDSHAKE | ERROR `unsupported-version` | Log; new HELLO after 5 minutes at the earliest | HANDSHAKE |
| READY | Framing error or HELLO\_REQUEST | Reset, send new HELLO | HANDSHAKE |
| READY | GOODBYE from the host | Reset | IDLE |
| IDLE | HELLO\_REQUEST | Send HELLO | HANDSHAKE |
| any open state | `POLLHUP` | Reset | WAITING |
| any open state | Session inactive | Send GOODBYE, close port | CLOSED |

Host:

| State | Event | Action | Next state |
| --- | --- | --- | --- |
| WAIT\_HELLO | Valid HELLO | Send WELCOME, POLICY | READY |
| WAIT\_HELLO | No HELLO 1 second after `VSERPORT_CHANGE` open | Send HELLO\_REQUEST | WAIT\_HELLO |
| READY | HELLO | Reset, send WELCOME, POLICY | READY |
| READY | Framing error | Reset, send HELLO\_REQUEST | WAIT\_HELLO |
| READY | GOODBYE from the agent | Reset | WAIT\_HELLO |
| any | `VSERPORT_CHANGE` closed | Reset, stop writing | WAIT\_HELLO |

## 6. Clipboard

Each side announces what it holds with an offer: a list of MIME types, no data. The other side fetches a format only when an application asks for it. Serials order the offers and settle the case where both sides copy at the same moment: the host wins.

Capabilities: `clipboard` for CLIPBOARD, `clipboard.primary` for PRIMARY.

### 6.1 Selections

| Value | Selection | Needs |
| --- | --- | --- |
| 0 | CLIPBOARD | capability `clipboard` |
| 1 | PRIMARY | capability `clipboard.primary` and POLICY `clipboard.primary` |

Other values are reserved. Receivers ignore clipboard messages with an unknown selection.

### 6.2 Ownership and serials

For each selection, each side keeps three values:

- `local_serial`: the serial of the last offer it sent, 0 if none;
- `peer_serial`: the serial of the last peer offer it accepted, 0 if none;
- `owner`: `local`, `peer` or `none`.

Sending an offer:

1. Increment `local_serial`.
2. Send CLIPBOARD\_OFFER with `serial` = `local_serial` and `seen` = `peer_serial`.
3. Set `owner` = `local`.

Receiving an offer with serial `s` and seen `x`:

1. If `s` ≤ `peer_serial`, the offer is stale: ignore it.
2. If `owner` = `local` and `x` < `local_serial`, the peer made its offer before it saw ours. This is a conflict, and the host wins:
   - the host ignores the agent's offer, but sets `peer_serial` = `s`;
   - the agent accepts the host's offer as in step 3.
3. Otherwise accept it: set `peer_serial` = `s` and `owner` = `peer`, and put a proxy for the offered types on the local clipboard.

A side that refuses an offer for its own policy reasons, for example the host's password filter, still sets `peer_serial` = `s`. If `owner` = `peer`, it also sets `owner` = `none` and removes its proxy, because the peer no longer holds that older content.

Local changes:

- When a local application takes the selection, the side sends an offer if the policy allows it and at least one type may be shared (Section 6.6).
- If it may not share the new content but `owner` = `local`, it sends CLIPBOARD\_RELEASE for its last offer instead.
- When the local selection is cleared and `owner` = `local`, it sends CLIPBOARD\_RELEASE and sets `owner` = `none`.
- Putting a proxy on the local clipboard changes that clipboard. A side MUST NOT answer that change with an offer of its own.

In a conflict, the copy made in the guest is replaced by the host's. Both sides end up with the host's content.

### 6.3 CLIPBOARD\_OFFER

**CLIPBOARD\_OFFER** (0x0100, both)

| Field | Type | Req. | Meaning |
| --- | --- | --- | --- |
| `selection` | `uint` | yes | Section 6.1. |
| `serial` | `uint` | yes | Strictly increasing per sender and selection, starting at 1 in each session. |
| `seen` | `uint` | yes | The peer serial the sender last accepted, 0 if none. |
| `types` | `[tstr]` | yes | 1 to 64 MIME types, in the source's order of preference. |
| `sizes` | `{tstr: uint}` | no | Sizes in bytes of types the sender already knows, for example prefetched text. |

### 6.4 CLIPBOARD\_RELEASE

**CLIPBOARD\_RELEASE** (0x0101, both) withdraws the sender's offer. The sender then sets `owner` = `none`.

| Field | Type | Req. | Meaning |
| --- | --- | --- | --- |
| `selection` | `uint` | yes | Section 6.1. |
| `serial` | `uint` | yes | The serial of the offer withdrawn. |

The receiver acts only if `owner` = `peer` and `peer_serial` = `serial`. It then sets `owner` = `none` and removes its proxy, if the proxy is still on the local clipboard.

### 6.5 Fetching data

The side that wants data sends CLIPBOARD\_REQUEST to the owner. The owner answers with one or more CLIPBOARD\_DATA, or with one CLIPBOARD\_FAIL.

**CLIPBOARD\_REQUEST** (0x0102, both)

| Field | Type | Req. | Meaning |
| --- | --- | --- | --- |
| `id` | `uint` | yes | Request id, increasing per requester, unique in the session. |
| `selection` | `uint` | yes | Section 6.1. |
| `serial` | `uint` | yes | The serial of the offer the request refers to. |
| `type` | `tstr` | yes | One of the offered types. |

**CLIPBOARD\_DATA** (0x0103, both)

| Field | Type | Req. | Meaning |
| --- | --- | --- | --- |
| `id` | `uint` | yes | The request id. |
| `data` | `bstr` | yes | The next chunk, at most 256 KiB. May be empty. |
| `size` | `uint` | no | Total size in bytes, in the first chunk, if known. |
| `last` | `bool` | no | `true` on the final chunk. Default `false`. |

**CLIPBOARD\_FAIL** (0x0104, both)

| Field | Type | Req. | Meaning |
| --- | --- | --- | --- |
| `id` | `uint` | yes | The request id. |
| `reason` | `tstr` | yes | `stale`, `not-offered`, `too-large`, `unavailable`, `denied` or `busy` (Appendix C). |
| `message` | `tstr` | no | Text for logs. |

**CLIPBOARD\_CANCEL** (0x0105, both) carries one field, `id` (`uint`). The requester sends it to abandon a request.

Rules:

- Chunks arrive in order. Their concatenation is the data.
- The total MUST NOT exceed the receiver's `limits.max_transfer`. If it would, the owner sends CLIPBOARD\_FAIL `too-large`, also in the middle of a transfer. The requester then discards what it has received.
- Chunks SHOULD be 64 KiB.
- A side with several transfers SHOULD alternate between them. It MUST let other queued messages pass between two chunks, so that PING or DISPLAY\_LAYOUT is not held up.
- A requester keeps at most 8 requests open. The owner MAY answer more with `busy`.
- After CANCEL the owner MUST stop and sends no answer. The requester ignores DATA and FAIL that still arrive for that id.
- The requester SHOULD cancel a request that receives nothing for 10 seconds.
- A request MAY be sent at any time while the offer is current, not only when an application pastes. The agent uses this to prefetch small text, so that it survives the source application quitting.
- After a new offer, the owner MAY still answer requests for the previous serial if it has the content. Otherwise it answers `stale`.

### 6.6 MIME types

- A type is a MIME type as the source system names it: printable ASCII, at most 255 bytes, normally with a `/`.
- X11 targets that are not MIME types, such as `TARGETS`, `MULTIPLE`, `TIMESTAMP`, `SAVE_TARGETS`, `UTF8_STRING`, `STRING` and `TEXT`, MUST NOT be sent.
- Text: if any text is available, the offer MUST include `text/plain;charset=utf-8`, and its data MUST be UTF-8. Other text types MAY be offered too.
- Images: if any image is available, the offer SHOULD include `image/png`, converted on request if the source lacks it.
- Types that start with `application/x-kemu-` are private to an implementation, for example markers against echoes. They MUST NOT be offered.
- Version 1 does not transfer files. Senders MUST NOT offer types that describe local file operations: `x-special/gnome-copied-files`, `application/x-kde-cutselection`, `application/vnd.portal.files` and `application/vnd.portal.filetransfer`.
- When serving `text/uri-list`, the owner MUST remove `file:` URIs. If none remain, it answers `unavailable`.
- `x-kde-passwordManagerHint` passes like any other type. It lets the receiver apply its own policy; KEMU uses it for its password filter.

The one type without a `/` that MUST pass is `x-kde-passwordManagerHint`, because KDE names it that way.

### 6.7 Rate limits

- PRIMARY changes with every text selection. A sender MUST NOT send more than one PRIMARY offer per 250 milliseconds and SHOULD send the latest state once the selection has stayed unchanged for that long.
- Some applications set the clipboard twice in a row. A sender SHOULD wait 50 milliseconds for the CLIPBOARD selection to settle before it offers it.

## 7. Display layout

The agent reports where each monitor sits in the guest's global coordinates, and which rectangle absolute pointer devices map onto. With that, the host can place the pointer on any monitor without guessing.

Capability: `display.layout`.

### 7.1 DISPLAY\_LAYOUT

**DISPLAY\_LAYOUT** (0x0200, G→H) is sent after the handshake and whenever the layout changes. The agent SHOULD wait until the layout has stayed unchanged for 100 milliseconds.

| Field | Type | Req. | Meaning |
| --- | --- | --- | --- |
| `generation` | `uint` | yes | Increases with every report in the session. |
| `space` | `{x: int, y: int, width: uint, height: uint}` | yes | The rectangle, in global coordinates, onto which the compositor maps absolute pointer devices. |
| `outputs` | `[output]` | yes | All outputs, enabled or not, at most 16. |
| `source` | `tstr` | no | Where the layout came from, for logs: `kwin`, `wlroots`, `mutter`, `xdg-output` or `x11-randr`. |

Each `output`:

| Field | Type | Req. | Meaning |
| --- | --- | --- | --- |
| `name` | `tstr` | yes | Connector name, for example `Virtual-1`. |
| `head` | `uint` | no | Scanout index of the display device (Section 7.3). Absent if the agent cannot tell. |
| `enabled` | `bool` | yes |  |
| `x`, `y` | `int` | if enabled | Top-left corner in global coordinates. |
| `width`, `height` | `uint` | if enabled | Size in global coordinates, after scaling and rotation. |
| `mode` | `{width: uint, height: uint, refresh: uint}` | no | Current mode in pixels; `refresh` in millihertz. |
| `scale` | `float` | no | Default 1.0. |
| `transform` | `tstr` | no | `normal`, `90`, `180`, `270`, `flipped`, `flipped-90`, `flipped-180` or `flipped-270`, as in `wl_output`. Default `normal`. |
| `primary` | `bool` | no | Default `false`. |

### 7.2 Global coordinates

- Wayland: the compositor's logical coordinates, as xdg-output reports them.
- X11: pixels of the root window.
- `space`: KWin maps absolute pointer devices onto the bounding rectangle of all outputs; an X server maps them onto the root window. If the agent cannot tell how its compositor does it, it reports the bounding rectangle of the enabled outputs.

### 7.3 Heads

- The Linux virtio-gpu driver creates one DRM connector per scanout, in scanout order, all of type `Virtual`.
- Connector numbers count per type across all DRM devices of the guest, so `Virtual-1` is not always head 0.
- The agent therefore takes the connectors of the virtio-gpu device, sorts them by their number, and assigns heads 0, 1, 2 in that order (Section 11.4).
- Compositors name outputs after the connector, for example `Virtual-2`. This links an output to its head.
- The agent MUST leave out `head` when it cannot map an output with certainty.
- The host maps a head to its D-Bus console through the console's `Head` property.

### 7.4 DISPLAY\_LAYOUT\_QUERY

**DISPLAY\_LAYOUT\_QUERY** (0x0201, H→G) has an empty body. The agent answers with a DISPLAY\_LAYOUT, with a new `generation`, even if nothing changed.

## 8. Pointer mapping on the host (non-normative)

With one tablet device and the reported layout, the host turns a pointer position on any monitor into one absolute tablet value. No pointer data crosses the agent port, and the guest needs no uinput and no root.

### 8.1 From viewer position to tablet value

For a pointer at pixel (px, py) in the framebuffer of head h, the image the viewer shows:

1. Find the enabled output with `head` = h. Without one, the host falls back to its own placement (the configured one, or the detected one in KEMU today).
2. Undo the output's `transform`, so that (px, py) refers to the output as the guest arranges it. For `normal` there is nothing to do.
3. Scale into global coordinates: gx = x + px × width / mode.width and gy = y + py × height / mode.height. Without `mode`, use the framebuffer size.
4. Convert into the tablet range: tx = (gx − space.x) × 32768 / space.width and ty = (gy − space.y) × 32768 / space.height, rounded down and limited to 0…32767.

Step 4 is the inverse of what the guest does. QEMU's tablets report 0 to 0x7FFF, and libinput spreads those 32768 steps over the target size.

### 8.2 Delivery to QEMU

| Path | Resolution | Note |
| --- | --- | --- |
| QMP `input-send-event` with `abs` events | Full: 32768 steps over `space` | Takes the raw range 0 to 0x7FFF. One command per motion, so coalesce to the viewer's frame rate. |
| D-Bus `SetAbsPosition` on the primary console | As many steps as the primary monitor is wide | QEMU scales by the primary console's size and refuses positions outside it. Two equal monitors side by side give 2-pixel steps. This is what KEMU's `MultiheadMouseMapping` uses today. |

Button and wheel events SHOULD take the same path as the motion they follow. Otherwise a click can overtake the motion before it.

### 8.3 Lifetime of the layout

- The host keeps the last layout across a new HELLO, because the monitors do not move with a protocol session.
- It drops the layout when the guest closes the port. The agent left with the graphical session, and a login screen or the next session may arrange the monitors differently; until an agent reports again, the host falls back to its own detection.
- A new session's first DISPLAY\_LAYOUT replaces it.
- With a single monitor no mapping is needed.

## 9. Reserved extensions

Version 1 reserves type ranges and capability names for two planned features. Neither is specified yet. Implementations MUST NOT use the reserved numbers until a later revision defines them.

| Range | Use |
| --- | --- |
| 0x0001–0x00FF | Core messages (Section 5) |
| 0x0100–0x01FF | Clipboard (Section 6) |
| 0x0200–0x02FF | Display layout (Section 7) |
| 0x0300–0x03FF | Reserved: display configuration |
| 0x0400–0x04FF | Reserved: file transfer |
| 0x0500–0x7FFF | Unassigned |
| 0x8000–0xFFFF | Experiments, never in released software |

### 9.1 Display configuration

Capability `display.configure`. The host asks the agent to apply modes, positions, scales or the primary output through the compositor's own interface:

- KWin: its output management protocol;
- wlroots compositors: `wlr-output-management`;
- GNOME: Mutter's `org.gnome.Mutter.DisplayConfig`;
- X11: RandR.

The agent answers with the result and a new DISPLAY\_LAYOUT. Uses: resize the guest with the viewer window on desktops that do not follow the preferred mode from virtio-gpu, and keep the monitor arrangement the same on host and guest.

Open questions: who wins when the guest user changes the settings, and whether the agent persists a configuration across guest reboots.

### 9.2 File transfer

Capability `file.transfer`, with data on the port `org.kde.kemu.agent.bulk.0`. Files and folders move in both directions, through the clipboard (`text/uri-list`, cut and copy) and through drag and drop.

- Control messages stay on the control port. Data uses the bulk port, so a large transfer does not hold up clipboard and layout.
- Incoming files land in a directory of the user's session and are then offered as `file:` URIs.
- Inside a shared folder (virtiofs), paths can be rewritten instead of copying the file.

Already known requirements: progress and cancel, size limits, confirmation on the host for files coming from the guest, no path outside the target directory, and no following of symbolic links.

## 10. Security considerations

The host treats everything from the guest as untrusted input. The agent trusts the host, which controls the VM anyway, but version 1 gives the host nothing beyond clipboard and layout.

### 10.1 Host

- Limits are checked before memory is allocated: frame length, CBOR depth, string lengths, at most 64 types and 16 outputs.
- A `size` field is a hint, never a reason to allocate. `max_transfer` is enforced on the bytes actually received.
- Guest clipboard content reaches host applications. The user chooses the directions per VM, and the host enforces them whatever the agent does.
- The host MAY drop any type from a guest offer before it puts the proxy on the host clipboard.
- The host MAY coalesce a burst of guest offers and apply only the latest.
- A false layout can only misplace the pointer inside the VM. The host still ignores sizes above 32768 and more than 16 outputs.

### 10.2 Guest

- Only the user of the active session can open the port (Sections 2.2 and 3.4). Other local users cannot read the clipboard through it.
- Limit: an agent whose session became inactive keeps its open file descriptor until it closes it. A hostile user in that session could hold the port and go on receiving host clipboard offers. Multi-user guests whose users do not trust each other are outside the threat model of version 1.
- The agent runs with the user's rights. In version 1 it executes nothing for the host and writes no files.
- Data the agent receives goes only to the clipboard of its own session.

### 10.3 Secrets and logs

- Password managers mark entries with `x-kde-passwordManagerHint` = `secret`. Both sides pass the hint on, and KEMU's password filter decides in both directions (default: on).
- Neither side logs clipboard content. Logs may contain types and sizes.
- HELLO carries the OS and desktop names, but no user name.

## 11. Implementation notes: Linux guest

The guest package ships one user program, one udev rule, one systemd user unit and one autostart entry. It needs Qt 6 and KGuiAddons, a Tier 1 framework that depends only on Qt.

### 11.1 Port access

`/usr/lib/udev/rules.d/70-kemu-agent.rules`:

```
SUBSYSTEM=="virtio-ports", ATTR{name}=="org.kde.kemu.agent.0", TAG+="uaccess"
```

The file name must sort before `73-seat-late.rules`, which applies `uaccess`.

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

- An XDG autostart entry with `X-systemd-skip=true` covers sessions that do not start systemd user units.
- The agent claims the session bus name `org.kde.kemu.Agent` and exits if another instance holds it.
- If the port does not exist, the agent exits with status 0: the guest is not a KEMU VM.

### 11.3 Clipboard backends

| Environment | Backend | Note |
| --- | --- | --- |
| KDE Plasma, Wayland | ext-data-control or wlr-data-control through KSystemClipboard | KWin offers data-control to clients outside a sandbox. KGuiAddons speaks ext-data-control from 6.14 on and prefers it; with an older KGuiAddons or a KWin that offers only the wlr protocol (Debian 13) the agent uses wlr-data-control. |
| wlroots compositors (Sway, Hyprland, labwc, Wayfire) | ext-data-control or wlr-data-control through KSystemClipboard | KGuiAddons falls back to the wlr protocol. |
| GNOME, Wayland | X11 through Xwayland (`QT_QPA_PLATFORM=xcb`) | Mutter offers no data-control. It bridges X11 and Wayland selections without focus; spice-vdagent relies on the same. |
| X11 desktops (Xfce, Cinnamon, MATE, Plasma X11) | `QClipboard` on xcb | XFixes reports selection changes without focus. |

The agent picks the backend at start, before Qt chooses a platform, from the protocols the compositor offers and the KGuiAddons version it was built with. A `QT_QPA_PLATFORM=wayland` set for the session is overridden where it would leave the agent without a clipboard. The agent reports its choice in HELLO `backends.clipboard`.

### 11.4 Heads

1. Find the DRM card whose `/sys/class/drm/cardN/device/driver` links to `virtio_gpu`.
2. List its connectors, the entries `/sys/class/drm/cardN-Virtual-M`.
3. Sort them by M. The k-th connector, counted from 0, is head k.
4. If there is not exactly one virtio-gpu card, leave `head` out.

### 11.5 Layout sources

| Environment | Source | `space` |
| --- | --- | --- |
| Wayland (KWin, wlroots) | `wl_output` name with xdg-output logical position and size | Bounding rectangle of the outputs |
| GNOME, Wayland | As above, over a Wayland connection of the agent's own | Bounding rectangle of the outputs |
| X11 | RandR outputs and CRTC geometry | Root window |

On GNOME the agent uses X11 for the clipboard, but it MUST NOT take the layout from Xwayland's RandR, which reports all monitors with one scale.

### 11.6 Echoes and prefetch

- The agent adds the private type `application/x-kemu-agent-proxy` to every proxy it installs. A clipboard change that carries this type is not offered.
- Right after a local copy, the agent prefetches text types up to 64 KiB. It keeps them for later requests and lists them in `sizes`.

### 11.7 Testing

A host simulator that speaks this protocol over a pipe lets the agent be tested in CI against a headless compositor, such as `sway` or `kwin_wayland --virtual`.

## 12. Implementation notes: KEMU host

KEMU replaces the `qemu-vdagent` chardev and the D-Bus clipboard path with one new port client, built like `GuestAgentClient`. The pointer code keeps `MultiheadMouseMapping` and feeds it the reported layout instead of detecting it.

### 12.1 What changes

| Area | Today | With kemu-agent |
| --- | --- | --- |
| QEMU command line (`qemucommandbuilder.cpp`) | `qemu-vdagent` chardev with `clipboard=on`, `mouse=on` for several virtio-gpu monitors, port `com.redhat.spice.0` | Socket chardev and port `org.kde.kemu.agent.0` (Section 3.2) |
| Clipboard (`QemuClipboardManager`, `org.qemu.Display1.Clipboard`) | UTF-8 text only; guest grabs without serial are dropped by QEMU | New client over this protocol: all MIME types, PRIMARY, password filter in both directions |
| Pointer with several monitors (`MultiheadMouseMapping`) | Layout detected from cursor feedback; spice-vdagent places the pointer when `mouse=on` | Layout from DISPLAY\_LAYOUT; detection stays as the fallback without an agent |
| QEMU capability check (`settingsdialog.cpp`) | Requires the `qemu-vdagent` chardev | Checks for `virtio-serial-pci` instead |
| Guest hint (`editvmdialog.cpp`) | Names spice-vdagent for several monitors | Names kemu-agent for Linux guests |
| Status in the viewer | — | The clipboard indicator shows whether kemu-agent carries the clipboard, with the agent version from HELLO |

### 12.2 Port client

- It owns one end of a socketpair; QEMU gets the other. Guest open and close come from `VSERPORT_CHANGE` for `kemuagent0`, as for qemu-guest-agent.
- The parser works incrementally, with scan mode (Section 5.3) and the 2-second timer for partial frames (Section 4.5).
- `QCborStreamReader` reads bodies and rejects items whose length is not known in advance. `QCborMap` writes them.
- Clipboard chunks are paced: no new chunk is queued while 128 KiB or more wait for the socket, so a control message waits behind at most that and one chunk.

### 12.3 Clipboard on the host

- POLICY follows the Devices → Clipboard menu. The password filter stays on the host and also covers guest offers (Section 6.2).
- Guest offers are fetched as soon as they arrive, up to 32 MiB per offer, and then placed on the host clipboard. `QMimeData` hands data to pasting applications synchronously, so the host cannot wait for the guest during a paste. Larger content stays in the guest.
- Host content is read into a snapshot when it is offered, within the same budget, and requests are served from it. Identical content is not offered again, since Wayland announces the selection anew on every focus change.
- The primary selection is shared only when the user switches it on and the host session has one.

### 12.4 Removing SPICE support

- Remove the `qemu-vdagent` block, its capability check, `QemuClipboardManager` with its tests, and the spice-vdagent pointer path in `MultiheadMouseMapping` (`setGuestAgentPointer`).
- Existing VMs need no configuration change: the port appears at the next start.
- A guest with only spice-vdagent loses clipboard sharing until kemu-agent is installed.
- KEMU MAY show a hint when no agent has connected 60 seconds after qemu-guest-agent did: the guest is up, but kemu-agent is missing.

## Appendix A. Message types

| Type | Name | Direction | Capability | Section |
| --- | --- | --- | --- | --- |
| 0x0001 | HELLO | G→H | core | 5.1 |
| 0x0002 | WELCOME | H→G | core | 5.1 |
| 0x0003 | HELLO\_REQUEST | H→G | core | 5.3 |
| 0x0004 | PING | both | core | 5.6 |
| 0x0005 | PONG | both | core | 5.6 |
| 0x0006 | GOODBYE | both | core | 5.7 |
| 0x0007 | ERROR | both | core | 5.8 |
| 0x0010 | POLICY | H→G | core | 5.5 |
| 0x0100 | CLIPBOARD\_OFFER | both | `clipboard` | 6.3 |
| 0x0101 | CLIPBOARD\_RELEASE | both | `clipboard` | 6.4 |
| 0x0102 | CLIPBOARD\_REQUEST | both | `clipboard` | 6.5 |
| 0x0103 | CLIPBOARD\_DATA | both | `clipboard` | 6.5 |
| 0x0104 | CLIPBOARD\_FAIL | both | `clipboard` | 6.5 |
| 0x0105 | CLIPBOARD\_CANCEL | both | `clipboard` | 6.5 |
| 0x0200 | DISPLAY\_LAYOUT | G→H | `display.layout` | 7.1 |
| 0x0201 | DISPLAY\_LAYOUT\_QUERY | H→G | `display.layout` | 7.4 |

## Appendix B. Capabilities

| Capability | Status | Meaning |
| --- | --- | --- |
| `clipboard` | defined | CLIPBOARD selection (Section 6) |
| `clipboard.primary` | defined | PRIMARY selection; needs `clipboard` |
| `display.layout` | defined | Layout reports (Section 7) |
| `display.configure` | reserved | Section 9.1 |
| `file.transfer` | reserved | Section 9.2 |

## Appendix C. Error codes

| Code | Used in | Meaning |
| --- | --- | --- |
| `unsupported-version` | ERROR | No common protocol version. Carries `versions` and `nonce`. |
| `bad-message` | ERROR | A required field is missing or has the wrong type. |
| `internal` | ERROR | The sender failed in a way the peer cannot fix. |
| `stale` | CLIPBOARD\_FAIL | The serial is no longer current, and the old content is gone. |
| `not-offered` | CLIPBOARD\_FAIL | The type was not in the offer. |
| `too-large` | CLIPBOARD\_FAIL | The data exceeds the requester's `max_transfer`. |
| `unavailable` | CLIPBOARD\_FAIL | The source application did not deliver, or nothing shareable remains. |
| `denied` | CLIPBOARD\_FAIL | The owner's policy forbids this transfer. |
| `busy` | CLIPBOARD\_FAIL | Too many open requests. |

## Appendix D. Examples

Bodies are in CBOR diagnostic notation; `h'…'` is a byte string.

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

The host does not list `clipboard.primary`, so PRIMARY is not shared in this session.

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

Before: the host owns CLIPBOARD with serial 5 and last accepted guest serial 2. The agent has accepted host serial 5; its own last serial is 2.

```
H→G CLIPBOARD_OFFER {"selection": 0, "serial": 6, "seen": 2, "types": […]}
G→H CLIPBOARD_OFFER {"selection": 0, "serial": 3, "seen": 5, "types": […]}
```

- Host: it owns the selection, and seen 5 < its serial 6. Conflict, the host wins: it ignores the offer and sets its peer serial to 3.
- Agent: it owns the selection, and seen 2 < its serial 3. Conflict: it accepts the host's offer and sets its peer serial to 6.
- Both now hold the host's content.

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

The pointer at pixel (1280, 720) of head 1, following Section 8.1:

- gx = 1920 + 1280 × 1280 / 2560 = 2560 and gy = 720 × 720 / 1440 = 360;
- tx = 2560 × 32768 / 3200 = 26214 and ty = 360 × 32768 / 1080 = 10922, rounded down.

## References

- [RFC 8949: Concise Binary Object Representation (CBOR)](https://www.rfc-editor.org/rfc/rfc8949)
- [RFC 2119](https://www.rfc-editor.org/rfc/rfc2119) and [RFC 8174](https://www.rfc-editor.org/rfc/rfc8174): requirement levels
- [ext-data-control-v1](https://wayland.app/protocols/ext-data-control-v1): Wayland clipboard protocol for privileged clients
- Checked in the sources (November 2026): Linux `drivers/char/virtio_console.c` (single open, `POLLHUP`, blocking writes) and `drivers/gpu/drm/virtio/virtgpu_display.c` (one connector per scanout); `drivers/gpu/drm/drm_connector.c` (connector numbers per type); QEMU `hw/char/virtio-console.c` and `hw/char/virtio-serial-bus.c` (open/close events, no reads while the guest port is closed), `qapi/ui.json` (`input-send-event` range 0 to 0x7FFF), `ui/dbus-console.c` (`SetAbsPosition` scaling), `ui/dbus-display1.xml` (console `Head` property); KWin `src/backends/libinput/connection.cpp` (absolute pointer mapped onto the workspace) and `src/backends/drm/drm_connector.cpp` (output names).
