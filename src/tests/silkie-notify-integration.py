"""Desktop notifications, Silkie's packed image formats and its depth attribute
end to end: a program behind goblin-skiff-server, the real client in a PTY, and
a test terminal that answers as Silkie does.

A notification is written by the program after an image that takes the link a
while to carry, and must reach the terminal before it."""
import base64
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
PATIENCE = float(os.environ.get("PATIENCE", "120"))
# The side of a square of noise, three bytes a pixel, that nothing compresses.
NOISE = int(os.environ.get("NOISE", "120"))


def osc(selector, fields):
    return b"\x1b]777;" + selector + b";" + fields + b"\x1b\\"


NOTIFY_ANSWER = (b"a=focus,report:o=always,unfocused,invisible:u=0,1,2:p=title,body,?,close,icon,alive,buttons:c=1:w=1:"
                 b"s=system,silent")
# What Silkie says to the client's probe.
SILKIE = [
    osc(b"caps", b"v=1:op=cap:id=7253:caps=3:ext=depth.1,caps.1,display.1,font.1,frame.1,latency.1:media=d,f,t,s:"
                 b"format=301,302,304,308,516,24,32,64,100,200,8,16,316,401,402,404,408,632,664"),
    osc(b"display", b"v=1:op=cap:id=7253:mode=0:kind=eink:color=gray:bits=4:gamut=srgb:headroom=1:scale=1:"
                    b"period=0:ppi=227:red_bits=0:green_bits=0:blue_bits=0:gray_bits=4:precision=configured"),
    b"\x1b]99;i=7253:p=?:r=goblin-skiff/probe/0;" + NOTIFY_ANSWER + b":r=1\x1b\\",
]
KITTY_QUERY = b"\x1b_Gi=4294967294,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\"

ROUTE = b"odin-seat/sessionA/3,odin-seat/sessionB/8"
NOTICE = b"\x1b]99;i=build:r=" + ROUTE + b":a=focus,report;Build finished\x1b\\"
PLAIN = b"\x1b]9;All 42 tests pass; none fail\x1b\\"
LEGACY = b"\x1b]777;notify;Deploy;done\x1b\\"
CLICKED = b"\x1b]99;i=build:r=" + ROUTE + b",silkie/terminalC/12:a=focus,report;\x1b\\"
CLOSED = b"\x1b]99;i=build:r=" + ROUTE + b":p=close;\x1b\\"
STATUS_QUERY = b"\x1b]7501;?\x1b\\"
STATUS_REPLY = b"\x1b]7501;?:states=idle,working,done,blocked,error:kinds=permission,question\x1b\\"
STATUS = b"\x1b]7501;state=done:app=build\x1b\\"
PROGRESS = b"\x1b]9;4;1;50\x1b\\"
# Sixteen by eight pixels, one bit each: a checkerboard of four-pixel squares.
PRECISION = b"\xff\x03\x00\xc0\x00\x02\x08\xe0"
BITS = (b"\xf0\xf0" * 4) + (b"\x0f\x0f" * 4)
DEPTH_TEXT = osc(b"depth", b"v=1:op=set:d=-150") + b"DEEP" + osc(b"depth", b"v=1:op=reset")

INBOX = bytearray()


def read_frame(prefix):
    """The next frame the program is sent that starts with prefix."""
    deadline = time.monotonic() + PATIENCE
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


def write_all(data):
    while data:
        data = data[os.write(1, data):]


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
    assert b"depth.1" in reply and b"format=301,302,304,308,516,24,32," in reply, reply
    os.write(1, osc(b"display", b"v=1:op=query:id=6"))
    display = read_frame(b"\x1b]777;display;")
    assert b"gray_bits=4" in display and b"precision=configured" in display, display

    # The query of the notification protocol is answered by the relay, with the route.
    os.write(1, b"\x1b]99;i=ask:p=?:r=" + ROUTE + b";\x1b\\")
    answer = read_frame(b"\x1b]99;")
    assert answer == b"\x1b]99;i=ask:p=?:r=" + ROUTE + b";" + NOTIFY_ANSWER + b":r=1\x1b\\", answer

    os.write(1, STATUS_QUERY)
    assert read_frame(b"\x1b]7501;") == STATUS_REPLY

    # An image the link takes a while over, and after it the notifications.
    noise = os.urandom(NOISE * NOISE * 3)
    image = b"\x1b[5;1H\x1b_Ga=T,i=9,f=24,s=%d,v=%d,q=2;" % (NOISE, NOISE) + base64.b64encode(noise) + b"\x1b\\"
    write_all(image + STATUS + NOTICE + PLAIN + LEGACY + PROGRESS + b"\x1b[1;1HAFTER_IMAGE")

    # The terminal says the notification was clicked, then that it closed.
    clicked = read_frame(b"\x1b]99;")
    assert clicked == CLICKED, clicked
    closed = read_frame(b"\x1b]99;")
    assert closed == CLOSED, closed
    while select.select([0], [], [], 0.3)[0]:
        INBOX.extend(os.read(0, 4096))
    assert not INBOX.replace(osc(b"caps", b"v=1:op=cap:id=5"), b""), ("stray input", bytes(INBOX))

    # Packed pixels and the depth of text.
    write_all(b"\x1b_Ga=T,i=7,f=301,s=16,v=8,c=2,r=1,q=2;" + base64.b64encode(BITS) + b"\x1b\\"
              + b"\x1b_Ga=T,i=8,f=632,E=16,G=2,s=2,v=1,c=2,r=1,q=2;" + base64.b64encode(PRECISION) + b"\x1b\\"
              + b"\x1b[2;1H" + DEPTH_TEXT + b"\x1b[3;1HANSWERS_OK")
    deadline = time.monotonic() + PATIENCE
    while os.read(0, 1) != b"q":
        assert time.monotonic() < deadline
    os.write(1, b"\x1b[4;1HFINISHED")


def session():
    spec = importlib.util.spec_from_file_location("graphics_relay", SCRIPT.with_name("control-panel-integration.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    build = Path.cwd()
    client = str((build / "../frontend/goblin-skiff-client").resolve())
    server = str((build / "../frontend/goblin-skiff-server").resolve())
    env = os.environ.copy()
    env.update(TERM="xterm-256color",
               MOSH_CLIENT_CAPS="keepalive-v1,sixel-state-v1,link-budget-v1,packed-state-v2,native-image-v1,packed-zstd-v1",
               MOSH_SERVER_NETWORK_TMOUT=str(int(PATIENCE) + 5))
    bootstrap = subprocess.run([server, "new", "-i", "127.0.0.1", "-c", "256", "--", sys.executable, str(SCRIPT),
                                "--remote"], stdin=subprocess.DEVNULL, capture_output=True, env=env, timeout=10)
    assert bootstrap.returncode == 0, bootstrap.stderr
    match = re.search(rb"MOSH CONNECT (\d+) (\S+)", bootstrap.stdout)
    assert match, bootstrap.stdout
    server_pid = int(re.search(rb"detached, pid = (\d+)", bootstrap.stderr)[1])
    relay = module.Relay(int(match[1]), fast=os.environ.get("RELAY", "fast") == "fast")
    env.update(MOSH_KEY=match[2].decode(), GOBLIN_SKIFF_SIXEL_STATE="1", GOBLIN_SKIFF_COMPACT_KEEPALIVE="1",
               GOBLIN_SKIFF_LINK_BUDGET="1", GOBLIN_SKIFF_MASCOT="none", MOSH_PREDICTION_DISPLAY="never",
               MOSH_NO_TERM_INIT="1")
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 80, 640, 512))
    process = subprocess.Popen([client, "127.0.0.1", str(relay.socket.getsockname()[1])], env=env,
                               stdin=slave, stdout=slave, stderr=slave, start_new_session=True,
                               preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
    os.close(slave)
    relay.thread.start()
    output, answered, seen = bytearray(), set(), {}

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
            for name, marker in (("notice", NOTICE), ("image", b"\x1b_Ga=t,q=2,f=32,i=9")):
                if name not in seen and marker in output:
                    seen[name] = time.monotonic()
            if b"\x1b]777;caps;v=1:op=query" in output and "probe" not in answered:
                os.write(master, b"".join(SILKIE))
                answered.add("probe")
            if STATUS_QUERY in output and "status" not in answered:
                os.write(master, STATUS_REPLY)
                answered.add("status")
            if KITTY_QUERY in output and "kitty" not in answered:
                os.write(master, b"\x1b_Gi=4294967294;OK\x1b\\")
                answered.add("kitty")
            if b"\x1b_Gi=4294967292,s=1,v=1,a=q,t=d,f=24,G=1;AAAA\x1b\\" in output and "gamut" not in answered:
                os.write(master, b"\x1b_Gi=4294967292;OK\x1b\\")
                answered.add("gamut")
            if b"\x1b[6n" in output and "cpr" not in answered:
                os.write(master, b"\x1b[20;1R")
                answered.add("cpr")

    def until(marker, start=0):
        deadline = time.monotonic() + PATIENCE
        while marker not in output[start:]:
            assert time.monotonic() < deadline, ("missing", marker, bytes(output[-1500:]))
            assert process.poll() is None, (process.returncode, bytes(output[-1500:]))
            collect()

    try:
        # The notifications, as the program wrote them, before the image written ahead of them.
        until(STATUS)
        assert "image" not in seen, "program status must pass bulk image traffic"
        until(NOTICE)
        assert "image" not in seen, "the notification did not wait for the image"
        until(PLAIN)
        until(LEGACY)
        text = bytes(output)
        assert text.find(STATUS) < text.find(NOTICE) < text.find(PLAIN) < text.find(LEGACY), "notifications keep their order"
        assert b"\x1b]99;i=ask" not in text, "the query is the relay's to answer"
        until(b"AFTER_IMAGE")
        until(PROGRESS)
        assert "image" in seen and seen["notice"] <= seen["image"], seen
        ahead = seen["image"] - seen["notice"]

        # What the terminal answers of a notification is the program's to read.
        start = len(output)
        os.write(master, CLICKED)
        os.write(master, CLOSED[:-2] + b"\x07")
        until(b"ANSWERS_OK", start)
        drawn = bytes(output[start:])
        packed = re.search(rb"\x1b_Ga=t,q=2,f=301,i=7,s=16,v=8[^;]*;([^\x1b]*)\x1b\\", drawn)
        assert packed and base64.b64decode(packed[1]) == BITS, ("packed pixels reach the terminal packed", drawn[-600:])
        precision = re.search(rb"\x1b_Ga=t,q=2,f=632,G=2,E=16,i=8,s=2,v=1[^;]*;([^\x1b]*)\x1b\\", drawn)
        assert precision and base64.b64decode(precision[1]) == PRECISION, ("precision pixels and curve reach terminal", drawn[-800:])
        assert osc(b"depth", b"v=1:op=set:d=-150:y=0") + b"DEEP" in drawn, ("the depth of text reaches the terminal", drawn[-600:])

        start = len(output)
        os.write(master, b"q")
        until(b"FINISHED", start)
        until(b"[goblin-skiff is exiting.]", start)
        process.wait(timeout=5)
        collect(0.1)
        assert process.returncode == 0, ("unclean shutdown", process.returncode, output[-1200:])
        print("PASS: notifications ahead of the screen (%.0f ms before an image of %d bytes written first), their "
              "return route and the terminal's answers, the query, packed pixels and depth through skiff"
              % (ahead * 1000, NOISE * NOISE * 3))
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
