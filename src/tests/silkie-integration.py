"""Silkie's OSC 777 extensions end to end: a program behind goblin-skiff-server,
the real client in a PTY, and a test terminal that answers as Silkie does."""
import errno
import fcntl
import importlib.util
import os
from pathlib import Path
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time
import tty

SCRIPT = Path(__file__).resolve()
SCROLL_ON = b"\x1b]777;scroll;v=1:op=mode:mode=1\x1b\\"
SCROLL_OFF = b"\x1b]777;scroll;v=1:op=mode:mode=0\x1b\\"
REPORT = b"\x1b]777;scroll;v=1:op=event:t=408211:dx=0:dy=250:x=8448:y=3072:mods=1:src=1:phase=2\x1b\\"
HAPTIC = b"\x1b]777;haptic;v=1:op=play:pattern=alignment\x1b\\"


def osc(selector, fields):
    return b"\x1b]777;" + selector + b";" + fields + b"\x1b\\"


# What Silkie says to the client's probe.
SILKIE = [
    osc(b"caps", b"v=1:op=cap:id=7253:caps=3:ext=caps.1,display.1,frame.1,gesture.1,haptic.1,ink.1,latency.1,"
                 b"scroll.1,tablet-pointer.1:media=d,f,t,s:format=24,32,64,100,200"),
    osc(b"tablet-pointer", b"v=1:op=cap:id=7253:caps=7:mode=0:w=640:h=512:cw=8:ch=16"),
    osc(b"scroll", b"v=1:op=cap:id=7253:caps=1:mode=0"),
    osc(b"haptic", b"v=1:op=cap:id=7253:caps=1"),
    osc(b"display", b"v=1:op=cap:id=7253:mode=0:kind=eink:color=gray:bits=4:gamut=srgb:headroom=1:scale=1:"
                    b"period=0:ppi=227"),
    osc(b"tablet-pointer", b"v=1:op=mode:mode=119"),
]


INBOX = bytearray()


def read_frame(prefix):
    """The next OSC 777 frame the program is sent that starts with prefix."""
    deadline = time.monotonic() + 20
    while True:
        start = INBOX.find(prefix)
        end = INBOX.find(b"\x1b\\", start) if start >= 0 else -1
        if end >= 0:
            frame = bytes(INBOX[start:end + 2])
            del INBOX[start:end + 2]
            return frame
        ready, _, _ = select.select([0], [], [], max(0, deadline - time.monotonic()))
        assert ready, ("remote input timeout", bytes(INBOX))
        INBOX.extend(os.read(0, 4096))


def remote():
    tty.setraw(0)
    # The client says what its terminal speaks once its probe is answered.
    reply = b""
    for _ in range(40):
        os.write(1, osc(b"caps", b"v=1:op=query:id=5"))
        ready, _, _ = select.select([0], [], [], 0.5)
        if ready:
            reply = read_frame(b"\x1b]777;caps;")
            break
    assert b"ext=caps.1," in reply and b"scroll.1" in reply and b"tablet-pointer.1" in reply, reply
    os.write(1, osc(b"display", b"v=1:op=query:id=6"))
    display = read_frame(b"\x1b]777;display;")
    assert b"kind=eink" in display and b"bits=4" in display, display
    os.write(1, osc(b"tablet-pointer", b"v=1:op=mode:mode=127") + osc(b"tablet-pointer", b"v=1:op=mode"))
    granted = read_frame(b"\x1b]777;tablet-pointer;")
    assert granted == osc(b"tablet-pointer", b"v=1:op=mode:mode=119"), granted
    os.write(1, osc(b"tablet-pointer", b"v=1:op=mode:mode=0") + SCROLL_ON + b"\x1b[1;1HSCROLL_ON")
    report = read_frame(b"\x1b]777;scroll;v=1:op=event")
    assert report == REPORT, report
    # Answers to the queries above were taken out; the terminal's replies to
    # the client's own probe must never have reached the program.
    while select.select([0], [], [], 0.3)[0]:
        INBOX.extend(os.read(0, 4096))
    assert not INBOX.replace(osc(b"caps", b"v=1:op=cap:id=5"), b""), ("stray input", bytes(INBOX))
    os.write(1, HAPTIC + b"\x1b[38:2:1:255:0:0m\x1b[56:250mBRIGHT\x1b[m\x1b[2;1HREPORT_OK")
    deadline = time.monotonic() + 20
    while os.read(0, 1) != b"q":
        assert time.monotonic() < deadline
    os.write(1, SCROLL_OFF + b"\x1b[3;1HFINISHED")


def session():
    spec = importlib.util.spec_from_file_location("graphics_relay", SCRIPT.with_name("control-panel-integration.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    build = Path.cwd()
    client = str((build / "../frontend/goblin-skiff-client").resolve())
    server = str((build / "../frontend/goblin-skiff-server").resolve())
    env = os.environ.copy()
    env.update(TERM="xterm-256color", MOSH_CLIENT_CAPS="keepalive-v1,sixel-state-v1", MOSH_SERVER_NETWORK_TMOUT="25")
    bootstrap = subprocess.run([server, "new", "-i", "127.0.0.1", "-c", "256", "--", sys.executable, str(SCRIPT),
                                "--remote"], stdin=subprocess.DEVNULL, capture_output=True, env=env, timeout=10)
    assert bootstrap.returncode == 0, bootstrap.stderr
    match = re.search(rb"MOSH CONNECT (\d+) (\S+)", bootstrap.stdout)
    assert match, bootstrap.stdout
    server_pid = int(re.search(rb"detached, pid = (\d+)", bootstrap.stderr)[1])
    relay = module.Relay(int(match[1]))
    env.update(MOSH_KEY=match[2].decode(), GOBLIN_SKIFF_SIXEL_STATE="1", GOBLIN_SKIFF_COMPACT_KEEPALIVE="1",
               GOBLIN_SKIFF_MASCOT="none", MOSH_PREDICTION_DISPLAY="never", MOSH_NO_TERM_INIT="1")
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 80, 640, 512))
    process = subprocess.Popen([client, "127.0.0.1", str(relay.socket.getsockname()[1])], env=env,
                               stdin=slave, stdout=slave, stderr=slave, start_new_session=True,
                               preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
    os.close(slave)
    relay.thread.start()
    output, answered = bytearray(), set()

    def collect(duration=0.1):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            readable, _, _ = select.select([master], [], [], max(0, deadline - time.monotonic()))
            if not readable:
                return
            try:
                data = os.read(master, 65536)
            except OSError as e:
                if e.errno != errno.EIO:
                    raise
                return
            if not data:
                return
            output.extend(data)
            if b"\x1b]777;caps;v=1:op=query" in output and "probe" not in answered:
                os.write(master, b"".join(SILKIE))
                answered.add("probe")
            if b"\x1b[6n" in output and "cpr" not in answered:
                os.write(master, b"\x1b[20;1R")
                answered.add("cpr")

    def until(marker, start=0):
        deadline = time.monotonic() + 25
        while marker not in output[start:]:
            assert time.monotonic() < deadline, ("missing", marker, bytes(output[-1500:]))
            assert process.poll() is None, (process.returncode, bytes(output[-1500:]))
            collect()

    try:
        until(b"SCROLL_ON")
        until(SCROLL_ON)
        start = len(output)
        os.write(master, REPORT)
        until(b"REPORT_OK", start)
        collect(0.2)
        assert HAPTIC in output[start:], "haptic feedback reaches the terminal"
        assert b"56:250" in output[start:], "HDR brightness reaches the terminal"
        assert b"38:2:1:255:0:0" in output[start:], "the gamut of a color reaches the terminal"
        start = len(output)
        os.write(master, b"q")
        until(b"FINISHED", start)
        until(SCROLL_OFF, start)
        until(b"[goblin-skiff is exiting.]", start)
        process.wait(timeout=5)
        collect(0.1)
        assert process.returncode == 0, ("unclean shutdown", process.returncode, output[-1200:])
        assert osc(b"display", b"v=1:op=mode:mode=0") in output, "the client turns its display reports off"
        print("PASS: capabilities, grants, scroll reports, haptics and HDR SGR through skiff")
    finally:
        os.close(master)
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
        relay.close()
        try:
            os.kill(server_pid, signal.SIGTERM)
        except ProcessLookupError:
            pass


if __name__ == "__main__":
    if sys.argv[1:2] == ["--remote"]:
        remote()
    else:
        session()
