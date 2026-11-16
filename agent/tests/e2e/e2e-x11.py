#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end test on X11: kemu-agent on Xvfb, the host played by
hostsim.Host on a pty. Guest applications are clipboardtool processes.

The second run sets WAYLAND_DISPLAY to a compositor that is not there, as a
stand-in for GNOME without data-control: the agent must fall back to X11.

    tests/e2e/e2e-x11.py <kemu-agent> <clipboardtool> [--verbose]
"""

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
from hostsim import GOODBYE, LAYOUT, OFFER  # noqa: E402

AGENT = os.path.abspath(sys.argv[1])
TOOL = os.path.abspath(sys.argv[2])
VERBOSE = "--verbose" in sys.argv
WORK = tempfile.mkdtemp(prefix="kemu-agent-x11-")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


xvfb = subprocess.Popen(["Xvfb", ":77", "-screen", "0", "1600x900x24", "-nolisten", "tcp"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1.0)
base = dict(os.environ, DISPLAY=":77", XDG_RUNTIME_DIR=WORK)
for key in ("WAYLAND_DISPLAY", "QT_QPA_PLATFORM"):
    base.pop(key, None)
os.chmod(WORK, 0o700)


def tool_get(env, mode, mtype):
    result = subprocess.run([TOOL, "get", mode, mtype], env=env, capture_output=True, timeout=10)
    return result.stdout if result.returncode == 0 else None


def tool_set(env, mode, mtype, data):
    path = os.path.join(WORK, "data-%d" % time.monotonic_ns())
    with open(path, "wb") as f:
        f.write(data)
    proc = subprocess.Popen([TOOL, "set", mode, mtype, path], env=env, stdout=subprocess.PIPE)
    proc.stdout.readline()  # "ready"
    return proc


def wait_for(fn, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = fn()
        if value:
            return value
        time.sleep(0.05)
    return fn()


def run(title, env):
    print("\n" + title, flush=True)
    master, slave = pty.openpty()
    tty.setraw(slave)
    host = hostsim.Host(master, verbose=VERBOSE)
    log = open(os.path.join(WORK, title.replace(" ", "_") + ".log"), "w")
    agent = subprocess.Popen([AGENT, "--port", os.ttyname(slave), "--verbose"], env=env, stdout=log, stderr=subprocess.STDOUT)
    holders = []
    try:
        hello = host.handshake(["clipboard", "clipboard.primary", "display.layout"]) or {}
        check(hello.get("backends", {}).get("clipboard") == "x11", "clipboard through X11")

        layout = host.expect(LAYOUT, timeout=5)
        check(layout is not None and layout.get("source") == "x11-randr", "layout from RandR")
        if layout:
            check(layout["space"] == {"x": 0, "y": 0, "width": 1600, "height": 900}, "space is the root window")
            enabled = [o for o in layout["outputs"] if o["enabled"]]
            check(len(enabled) == 1 and enabled[0].get("primary") is True, "one primary output")

        host.drain(0.3)
        holders.append(tool_set(env, "clipboard", "text/plain", b"x11 text"))
        offer = host.expect(OFFER, timeout=5)
        check(offer is not None and "text/plain;charset=utf-8" in offer["types"], "guest copy offered")
        if offer:
            data, failure = host.fetch(offer, "text/plain;charset=utf-8")
            check(data == b"x11 text", "guest text arrives (%s)" % (failure or "ok"))

        holders.append(tool_set(env, "primary", "text/html", b"<b>sel</b>"))
        offer = host.expect(OFFER, timeout=5, where=lambda b: b["selection"] == 1)
        check(offer is not None and "text/html" in offer["types"], "guest PRIMARY offered")
        if offer:
            data, failure = host.fetch(offer, "text/html")
            check(data == b"<b>sel</b>", "guest PRIMARY arrives (%s)" % (failure or "ok"))

        contents = {"text/plain;charset=utf-8": "host äöü".encode(), "text/html": b"<i>host</i>"}
        host.offer(0, contents)
        host.serve(contents)
        check(wait_for(lambda: tool_get(env, "clipboard", "text/plain")) == contents["text/plain;charset=utf-8"], "host text pasted")
        check(tool_get(env, "clipboard", "text/html") == contents["text/html"], "host HTML pasted")
        check(host.none_of(OFFER, wait=0.5), "not offered back")

        png = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "pixel.png"), "rb").read()
        host.offer(0, {"image/png": png})
        host.serve({"image/png": png})
        pasted = wait_for(lambda: tool_get(env, "clipboard", "image/png"))
        check(pasted is not None and pasted.startswith(b"\x89PNG"), "host image pasted")

        host.offer(1, {"text/plain;charset=utf-8": b"host primary"})
        host.serve({"text/plain;charset=utf-8": b"host primary"})
        check(wait_for(lambda: tool_get(env, "primary", "text/plain")) == b"host primary", "host PRIMARY pasted")

        agent.send_signal(signal.SIGTERM)
        check(host.expect(GOODBYE, timeout=5) is not None, "GOODBYE on SIGTERM")
        check(agent.wait(timeout=5) == 0, "exits cleanly")
    finally:
        if agent.poll() is None:
            agent.kill()
        for proc in holders:
            if proc.poll() is None:
                proc.kill()
        host.close()
        log.close()
        os.close(master)
        os.close(slave)


try:
    run("X11 session", base)
    run("Wayland without data-control", dict(base, WAYLAND_DISPLAY="wayland-absent"))
finally:
    xvfb.terminate()
    xvfb.wait()

print()
if failures:
    print("%d check(s) failed; logs in %s" % (len(failures), WORK))
    sys.exit(1)
print("all checks passed")
shutil.rmtree(WORK, ignore_errors=True)
