#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end test: kemu-agent in a headless sway session, the host played
by hostsim.Host on the master of a pty that stands in for the virtio port.
Guest applications are wl-copy and wl-paste.

    tests/e2e/e2e-wayland.py <path to kemu-agent> [--verbose]
"""

import glob
import os
import pty
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import tty

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostsim  # noqa: E402
from hostsim import (CANCEL, DATA, GOODBYE, HELLO, HELLO_REQUEST, LAYOUT, LAYOUT_QUERY, OFFER,  # noqa: E402
                     RELEASE)

AGENT = os.path.abspath(sys.argv[1])
VERBOSE = "--verbose" in sys.argv
WORK = tempfile.mkdtemp(prefix="kemu-agent-e2e-")
os.chmod(WORK, 0o700)
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


def section(title):
    print("\n" + title, flush=True)


# --- Headless sway with two outputs ------------------------------------------------

env = dict(os.environ)
for key in ("WAYLAND_DISPLAY", "DISPLAY", "SWAYSOCK", "QT_QPA_PLATFORM"):
    env.pop(key, None)
env.update(XDG_RUNTIME_DIR=WORK, WLR_BACKENDS="headless", WLR_RENDERER="pixman", WLR_LIBINPUT_NO_DEVICES="1")
cfg = os.path.join(WORK, "sway.cfg")
with open(cfg, "w") as f:
    f.write("output HEADLESS-1 resolution 1920x1080 position 0 0\n")
sway = subprocess.Popen(["sway", "-c", cfg], env=env, stdout=open(os.path.join(WORK, "sway.log"), "w"), stderr=subprocess.STDOUT)
for _ in range(200):
    if os.path.exists(os.path.join(WORK, "wayland-1")) and glob.glob(os.path.join(WORK, "sway-ipc.*.sock")):
        break
    time.sleep(0.05)
wenv = dict(env, WAYLAND_DISPLAY="wayland-1", SWAYSOCK=glob.glob(os.path.join(WORK, "sway-ipc.*.sock"))[0])


def swaymsg(*args):
    return subprocess.run(["swaymsg", *args], env=wenv, capture_output=True, text=True).stdout


swaymsg("create_output")
swaymsg("output", "HEADLESS-2", "resolution", "2560x1440", "scale", "2", "position", "1920", "0")


def wl_copy(*args, data=None):
    subprocess.run(["wl-copy", *args], env=wenv, input=data, check=True)


def wl_paste(*args):
    result = subprocess.run(["wl-paste", "--no-newline", *args], env=wenv, capture_output=True, timeout=10)
    return result.stdout if result.returncode == 0 else None


def wl_types(*args):
    result = subprocess.run(["wl-paste", "--list-types", *args], env=wenv, capture_output=True, text=True, timeout=10)
    return result.stdout.split() if result.returncode == 0 else []


def wait_for(fn, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = fn()
        if value:
            return value
        time.sleep(0.05)
    return fn()


# --- The port and the agent --------------------------------------------------------

master, slave = pty.openpty()
tty.setraw(slave)
port_path = os.ttyname(slave)
host = hostsim.Host(master, verbose=VERBOSE)
agent_log = open(os.path.join(WORK, "agent.log"), "w")
agent = subprocess.Popen([AGENT, "--port", port_path, "--verbose"], env=wenv, stdout=agent_log, stderr=subprocess.STDOUT)
CAPS = ["clipboard", "clipboard.primary", "display.layout"]

try:
    section("Handshake")
    hello = host.handshake(CAPS)
    check(hello is not None, "HELLO arrives")
    hello = hello or {}
    check(len(hello.get("nonce", b"")) == 16, "16-byte nonce")
    check(hello.get("versions") == [1], "offers version 1")
    check(set(CAPS) <= set(hello.get("capabilities", [])), "announces clipboard, PRIMARY and layout")
    backend = hello.get("backends", {}).get("clipboard")
    check(backend in ("ext-data-control", "wlr-data-control"), "clipboard through data-control (%s)" % backend)
    check(hello.get("session", {}).get("type") == "wayland", "Wayland session")

    section("Display layout")
    layout = host.expect(LAYOUT, timeout=5)
    check(layout is not None, "DISPLAY_LAYOUT after the handshake")
    if layout:
        outputs = {o["name"]: o for o in layout["outputs"]}
        one, two = outputs.get("HEADLESS-1", {}), outputs.get("HEADLESS-2", {})
        check((one.get("x"), one.get("y"), one.get("width"), one.get("height")) == (0, 0, 1920, 1080), "HEADLESS-1 at 0,0, 1920x1080")
        check((two.get("x"), two.get("y"), two.get("width"), two.get("height")) == (1920, 0, 1280, 720), "HEADLESS-2 at 1920,0, 1280x720 logical")
        check(two.get("mode", {}).get("width") == 2560 and two.get("scale") == 2.0, "HEADLESS-2 mode 2560 wide, scale 2")
        check(layout["space"] == {"x": 0, "y": 0, "width": 3200, "height": 1080}, "space 3200x1080")
        check("head" not in two, "no head for a non-virtio connector")
        check(layout.get("source") == "xdg-output", "layout from xdg-output")
        generation = layout["generation"]

    swaymsg("output", "HEADLESS-2", "position", "0", "1080")
    moved = host.expect(LAYOUT, timeout=5)
    check(moved is not None and moved["generation"] > generation, "a moved output is reported again")
    if moved:
        two = {o["name"]: o for o in moved["outputs"]}.get("HEADLESS-2", {})
        check((two.get("x"), two.get("y")) == (0, 1080) and moved["space"]["height"] == 1800, "new position and space")
    host.send(LAYOUT_QUERY)
    check(host.expect(LAYOUT, timeout=3) is not None, "DISPLAY_LAYOUT_QUERY answered")

    section("Guest to host")
    host.drain()
    wl_copy(data=b"hello from the guest")
    offer = host.expect(OFFER, timeout=5)
    check(offer is not None and offer["selection"] == 0, "CLIPBOARD offer after wl-copy")
    if offer:
        check("text/plain;charset=utf-8" in offer["types"], "offers UTF-8 text")
        data, failure = host.fetch(offer, "text/plain;charset=utf-8")
        check(data == b"hello from the guest", "text arrives (%s)" % (failure or "ok"))

    wl_copy("--type", "text/html", data=b"<b>bold</b>")
    offer = host.expect(OFFER, timeout=5)
    check(offer is not None and "text/html" in offer["types"], "HTML offered")
    if offer:
        data, failure = host.fetch(offer, "text/html")
        check(data == b"<b>bold</b>", "HTML arrives (%s)" % (failure or "ok"))

    big = os.urandom(5 * 1024 * 1024)
    wl_copy("--type", "application/x-kemu-test-blob", data=big)
    release = host.expect(RELEASE, timeout=5)
    check(release is not None and host.none_of(OFFER, wait=0.3), "a private application/x-kemu-* type stays home; the old offer is withdrawn")
    wl_copy("--type", "application/octet-stream", data=big)
    offer = host.expect(OFFER, timeout=5)
    if offer:
        started = time.monotonic()
        data, failure = host.fetch(offer, "application/octet-stream", timeout=30)
        check(data == big, "5 MiB binary arrives intact (%s, %.1f s)" % (failure or "ok", time.monotonic() - started))
    else:
        check(False, "binary offered")

    wl_copy("--primary", data=b"selected text")
    offer = host.expect(OFFER, timeout=5, where=lambda b: b["selection"] == 1)
    check(offer is not None, "PRIMARY offered")
    if offer:
        data, failure = host.fetch(offer, "text/plain;charset=utf-8")
        check(data == b"selected text", "PRIMARY text arrives (%s)" % (failure or "ok"))

    section("Host to guest")
    host.drain()
    contents = {"text/html": b"<i>from the host</i>", "text/plain;charset=utf-8": "from the host ✓".encode()}
    host.offer(0, contents)
    served = host.serve(contents)
    check(sorted(served) == sorted(contents), "agent fetches both types")
    pasted = wait_for(lambda: wl_paste())
    check(pasted == contents["text/plain;charset=utf-8"], "wl-paste gets the text")
    check(wl_paste("--type", "text/html") == contents["text/html"], "wl-paste gets the HTML")
    check(host.none_of(OFFER, wait=0.5), "the host's content is not offered back")

    png = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "pixel.png"), "rb").read()
    images = {"image/png": png, "image/bmp": b"BM-not-needed"}
    host.offer(0, images)
    served = host.serve(images)
    check(served == ["image/png"], "one image format fetched (%s)" % served)
    # A clipboard that serves images through QImage hands out the same
    # picture re-encoded, so compare the PNG header (size, depth, colour type).
    pasted_png = wait_for(lambda: wl_paste("--type", "image/png")) or b""
    check(pasted_png == png or (pasted_png[:8] == png[:8] and pasted_png[16:26] == png[16:26]), "wl-paste gets the PNG")
    types = wl_types()
    check("image/jpeg" in types or "image/bmp" in types, "guest offers further image formats")

    blob = os.urandom(3 * 1024 * 1024 + 11)
    host.offer(0, {"application/x-large": blob})
    host.serve({"application/x-large": blob}, timeout=30)
    check(wait_for(lambda: wl_paste("--type", "application/x-large"), timeout=10) == blob, "3 MiB custom type pasted intact")

    host.offer(1, {"text/plain;charset=utf-8": b"host selection"})
    host.serve({"text/plain;charset=utf-8": b"host selection"})
    check(wait_for(lambda: wl_paste("--primary")) == b"host selection", "PRIMARY from the host")

    serial = host.offer(0, {"text/plain;charset=utf-8": b"short-lived"})
    host.serve({"text/plain;charset=utf-8": b"short-lived"})
    check(wait_for(lambda: wl_paste()) == b"short-lived", "text in place before release")
    host.send(RELEASE, {"selection": 0, "serial": serial})
    check(wait_for(lambda: wl_paste() is None, timeout=3), "CLIPBOARD_RELEASE clears it")

    section("Conflicts and cancels")
    host.drain()
    wl_copy(data=b"guest wins?")
    guest_offer = host.expect(OFFER, timeout=5)
    # The host copied before it saw the guest's offer: seen stays behind.
    host.offer(0, {"text/plain;charset=utf-8": b"host wins"}, seen=0)
    host.serve({"text/plain;charset=utf-8": b"host wins"})
    check(wait_for(lambda: wl_paste() == b"host wins"), "host wins a simultaneous copy")

    slow = {"text/plain;charset=utf-8": b"never delivered"}
    host.offer(0, slow)
    request = host.expect(hostsim.REQUEST, timeout=5)
    wl_copy(data=b"newer guest copy")
    cancel = host.expect(CANCEL, timeout=5)
    check(request is not None and cancel is not None and cancel["id"] == request["id"], "a guest copy cancels the open fetch")
    check(host.expect(OFFER, timeout=5) is not None, "and is offered")
    host.send(DATA, {"id": request["id"] if request else 0, "data": b"late", "last": True})
    time.sleep(0.3)
    check(wl_paste() == b"newer guest copy", "a late answer does not overwrite it")

    section("Resynchronisation")
    host.drain()
    host.send_raw(b"\x00garbage KEMU\x02\x00 more garbage")
    hello = host.expect(HELLO, timeout=5)
    check(hello is not None, "framing error leads to a new HELLO")
    host.send_raw(hostsim.frame(hostsim.WELCOME, {"nonce": b"x" * 16, "version": 1, "capabilities": CAPS}))
    host.send(hostsim.WELCOME, {"nonce": hello["nonce"] if hello else b"", "version": 1, "host": {"name": "KEMU"}, "capabilities": CAPS})
    host.policy(True, True, True)
    host.serial = [0, 0]
    check(host.expect(LAYOUT, timeout=5) is not None, "session back after resync")
    host.drain()
    host.send(HELLO_REQUEST)
    hello = host.handshake(CAPS)
    check(hello is not None, "HELLO_REQUEST restarts the session")
    offer = host.expect(OFFER, timeout=5)
    check(offer is not None and offer["serial"] == 1, "the held selection is offered again, from serial 1")
    wl_copy(data=b"after restart")
    offer = host.expect(OFFER, timeout=5)
    check(offer is not None and offer["serial"] == 2, "serials go on in the new session")

    section("Shutdown")
    agent.send_signal(signal.SIGTERM)
    goodbye = host.expect(GOODBYE, timeout=5)
    check(goodbye is not None and goodbye.get("reason") == "shutdown", "GOODBYE on SIGTERM")
    check(agent.wait(timeout=5) == 0, "agent exits cleanly")
    check(not host.errors, "no framing errors on the host side %s" % host.errors)
finally:
    if agent.poll() is None:
        agent.kill()
    host.close()
    sway.terminate()
    sway.wait()
    agent_log.close()

print()
if failures:
    print("%d check(s) failed; logs in %s" % (len(failures), WORK))
    sys.exit(1)
print("all checks passed")
shutil.rmtree(WORK, ignore_errors=True)
