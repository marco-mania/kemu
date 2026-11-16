# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
# SPDX-License-Identifier: GPL-3.0-or-later
"""The host end of the KEMU agent protocol, for tests: frames over a file
descriptor (the master of a pty that stands in for the virtio port)."""

import os
import queue
import select
import struct
import threading
import time

import cbor2

MAGIC = b"KEMU"
HELLO, WELCOME, HELLO_REQUEST, PING, PONG, GOODBYE, ERROR, POLICY = 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x10
OFFER, RELEASE, REQUEST, DATA, FAIL, CANCEL = 0x100, 0x101, 0x102, 0x103, 0x104, 0x105
LAYOUT, LAYOUT_QUERY = 0x200, 0x201
NAMES = {
    HELLO: "HELLO", WELCOME: "WELCOME", HELLO_REQUEST: "HELLO_REQUEST", PING: "PING", PONG: "PONG",
    GOODBYE: "GOODBYE", ERROR: "ERROR", POLICY: "POLICY", OFFER: "CLIPBOARD_OFFER",
    RELEASE: "CLIPBOARD_RELEASE", REQUEST: "CLIPBOARD_REQUEST", DATA: "CLIPBOARD_DATA",
    FAIL: "CLIPBOARD_FAIL", CANCEL: "CLIPBOARD_CANCEL", LAYOUT: "DISPLAY_LAYOUT",
    LAYOUT_QUERY: "DISPLAY_LAYOUT_QUERY",
}
CHUNK = 64 * 1024


def frame(mtype, body=None):
    data = cbor2.dumps(body) if body else b""
    return MAGIC + struct.pack("<HHI", mtype, 0, len(data)) + data


class Host:
    def __init__(self, fd, verbose=False):
        self.fd = fd
        self.verbose = verbose
        self.frames = queue.Queue()
        self.pending = []
        self.errors = []
        self.serial = [0, 0]
        self.next_id = 0
        self._stop = False
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._read, daemon=True)
        self._thread.start()

    def _read(self):
        buf = b""
        while not self._stop:
            ready, _, _ = select.select([self.fd], [], [], 0.1)
            if not ready:
                continue
            try:
                chunk = os.read(self.fd, 1 << 20)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while len(buf) >= 12:
                if buf[:4] != MAGIC:
                    self.errors.append("framing error: %r" % buf[:16])
                    buf = b""
                    break
                mtype, _flags, length = struct.unpack("<HHI", buf[4:12])
                if len(buf) < 12 + length:
                    break
                body = cbor2.loads(buf[12:12 + length]) if length else {}
                buf = buf[12 + length:]
                if self.verbose:
                    print("    <-", NAMES.get(mtype, hex(mtype)), _short(body), flush=True)
                self.frames.put((mtype, body))

    def close(self):
        self._stop = True

    def send(self, mtype, body=None):
        self.send_raw(frame(mtype, body))
        if self.verbose:
            print("    ->", NAMES.get(mtype, hex(mtype)), _short(body or {}), flush=True)

    def send_raw(self, data):
        with self._lock:
            view = memoryview(data)
            while view:
                written = os.write(self.fd, view)
                view = view[written:]

    def expect(self, mtype, timeout=5.0, where=None):
        """The first frame of this type (and matching `where`) that arrives;
        frames before it are kept for later calls."""
        deadline = time.monotonic() + timeout
        for i, (t, body) in enumerate(self.pending):
            if t == mtype and (where is None or where(body)):
                return self.pending.pop(i)[1]
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                t, body = self.frames.get(timeout=remaining)
            except queue.Empty:
                return None
            if t == mtype and (where is None or where(body)):
                return body
            self.pending.append((t, body))

    def none_of(self, mtype, wait=0.5):
        return self.expect(mtype, timeout=wait) is None

    def drain(self, wait=0.2):
        time.sleep(wait)
        self.pending.clear()
        while not self.frames.empty():
            self.frames.get_nowait()

    # --- Handshake ------------------------------------------------------------

    def handshake(self, capabilities, to_host=True, to_guest=True, primary=True, timeout=10.0):
        hello = self.expect(HELLO, timeout)
        if hello is None:
            return None
        self.send(WELCOME, {"nonce": hello["nonce"], "version": 1,
                            "host": {"name": "KEMU", "version": "test"},
                            "capabilities": capabilities})
        self.policy(to_host, to_guest, primary)
        self.serial = [0, 0]
        return hello

    def policy(self, to_host, to_guest, primary):
        self.send(POLICY, {"clipboard": {"to_host": to_host, "to_guest": to_guest, "primary": primary}})

    # --- Clipboard --------------------------------------------------------------

    def fetch(self, offer, mtype, timeout=10.0):
        """Requests one type of a guest offer; returns (bytes, failure)."""
        self.next_id += 1
        rid = self.next_id
        self.send(REQUEST, {"id": rid, "selection": offer["selection"], "serial": offer["serial"], "type": mtype})
        data = b""
        while True:
            answer = self._expect_answer(rid, timeout)
            if answer is None:
                return None, "timeout"
            kind, body = answer
            if kind == FAIL:
                return None, body["reason"]
            data += body["data"]
            if body.get("last"):
                return data, None

    def _expect_answer(self, rid, timeout):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for kind in (DATA, FAIL):
                body = self.expect(kind, timeout=0.05, where=lambda b: b.get("id") == rid)
                if body is not None:
                    return kind, body
        return None

    def offer(self, selection, contents, seen=0):
        """Offers {type: bytes} and serves the agent's requests until it has
        asked for everything it wants. Returns the types requested."""
        self.serial[selection] += 1
        serial = self.serial[selection]
        self.send(OFFER, {"selection": selection, "serial": serial, "seen": seen,
                          "types": list(contents), "sizes": {t: len(d) for t, d in contents.items()}})
        return serial

    def serve(self, contents, quiet=1.0, timeout=10.0):
        """Answers requests until none came for `quiet` seconds."""
        served = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            request = self.expect(REQUEST, timeout=quiet)
            if request is None:
                break
            mtype = request["type"]
            served.append(mtype)
            if mtype not in contents:
                self.send(FAIL, {"id": request["id"], "reason": "not-offered"})
                continue
            data = contents[mtype]
            offset = 0
            while True:
                chunk = data[offset:offset + CHUNK]
                body = {"id": request["id"], "data": chunk}
                if offset == 0:
                    body["size"] = len(data)
                offset += len(chunk)
                if offset >= len(data):
                    body["last"] = True
                self.send(DATA, body)
                if offset >= len(data):
                    break
        return served


def _short(body):
    def shorten(value):
        if isinstance(value, bytes):
            return "<%d bytes>" % len(value)
        if isinstance(value, dict):
            return {k: shorten(v) for k, v in value.items()}
        if isinstance(value, list):
            return [shorten(v) for v in value]
        return value
    return shorten(body)


def main():
    """Plays the host for a real VM, before KEMU speaks the protocol:

        qemu ... -device virtio-serial-pci \\
                 -chardev socket,id=kemuagent,path=/tmp/kemu-agent.sock,server=on,wait=off \\
                 -device virtserialport,chardev=kemuagent,name=org.kde.kemu.agent.0
        python3 hostsim.py /tmp/kemu-agent.sock

    Prints what the agent sends and the text the guest copies; every line
    typed here is offered to the guest clipboard."""
    import socket
    import sys

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.connect(sys.argv[1])
    host = Host(sock.fileno(), verbose=True)
    host.send(HELLO_REQUEST)
    if host.handshake(["clipboard", "clipboard.primary", "display.layout"], timeout=60) is None:
        print("no HELLO from the agent")
        return 1

    utf8 = "text/plain;charset=utf-8"
    ours = {}
    fetches = {}
    while True:
        ready, _, _ = select.select([sys.stdin], [], [], 0.1)
        if ready:
            line = sys.stdin.readline()
            if not line:
                return 0
            ours = {utf8: line.rstrip("\n").encode()}
            host.offer(0, ours)
        while True:
            try:
                mtype, body = host.frames.get_nowait()
            except queue.Empty:
                break
            if mtype == OFFER and utf8 in body["types"]:
                host.next_id += 1
                fetches[host.next_id] = b""
                host.send(REQUEST, {"id": host.next_id, "selection": body["selection"], "serial": body["serial"], "type": utf8})
            elif mtype == REQUEST:
                data = ours.get(body["type"])
                if data is None:
                    host.send(FAIL, {"id": body["id"], "reason": "not-offered"})
                else:
                    host.send(DATA, {"id": body["id"], "data": data, "size": len(data), "last": True})
            elif mtype == DATA and body["id"] in fetches:
                fetches[body["id"]] += body["data"]
                if body.get("last"):
                    print("guest copied:", fetches.pop(body["id"]).decode(errors="replace"), flush=True)
            elif mtype == FAIL and body["id"] in fetches:
                fetches.pop(body["id"])
                print("guest could not deliver:", body["reason"], flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
