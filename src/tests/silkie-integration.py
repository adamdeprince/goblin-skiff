"""Silkie's OSC 777 extensions end to end: a program behind goblin-skiff-server,
the real client in a PTY, and a test terminal that answers as Silkie does."""
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
SCROLL_ON = b"\x1b]777;scroll;v=1:op=mode:mode=1\x1b\\"
SCROLL_OFF = b"\x1b]777;scroll;v=1:op=mode:mode=0\x1b\\"
REPORT = b"\x1b]777;scroll;v=1:op=event:t=408211:dx=0:dy=250:x=8448:y=3072:mods=1:src=1:phase=2\x1b\\"
HAPTIC = b"\x1b]777;haptic;v=1:op=play:pattern=alignment\x1b\\"


def osc(selector, fields):
    return b"\x1b]777;" + selector + b";" + fields + b"\x1b\\"


# What Silkie says to the client's probe.
SILKIE = [
    osc(b"caps", b"v=1:op=cap:id=7253:caps=3:ext=caps.1,display.1,font.1,frame.1,gesture.1,haptic.1,ink.1,latency.1,"
                 b"scroll.1,tablet-pointer.1:media=d,f,t,s:format=24,32,64,100,200:graphics=viewport.1"),
    osc(b"font", b"v=1:op=cap:id=7253:caps=1:roles=default,sans,serif:weight_min=100:weight_max=900:"
                 b"slant_min=-1500:slant_max=1500:synthetic_slant=1"),
    osc(b"tablet-pointer", b"v=1:op=cap:id=7253:caps=7:mode=0:w=640:h=512:cw=8:ch=16"),
    osc(b"scroll", b"v=1:op=cap:id=7253:caps=1:mode=0"),
    osc(b"haptic", b"v=1:op=cap:id=7253:caps=1"),
    osc(b"display", b"v=1:op=cap:id=7253:mode=0:kind=eink:color=gray:bits=4:gamut=srgb:headroom=1:scale=1:"
                    b"period=0:ppi=227"),
    osc(b"tablet-pointer", b"v=1:op=mode:mode=119"),
]


INBOX = bytearray()
PATIENCE = float(os.environ.get("PATIENCE", "120"))
# Enough to pile up many times over between packets, and for the relay's losses
# to make a whole update in one pass all but impossible.
POINTS = int(os.environ.get("POINTS", "1200"))
PEN_MODE = osc(b"tablet-pointer", b"v=1:op=mode:mode=1")
XTERM_ON = b"\x1b[?1003h"
SERVICE_ANSWER = base64.b64encode(b'{"version":1}')
KITTY_QUERY = b"\x1b_Gi=4294967294,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\"
SESSION_FONTS = os.environ.get("SESSION_FONTS", "1") == "1"
FONT_UPLOAD = b'{"op":"upload","id":"body","data":"AAAA"}'
FONT_RELEASE = b'{"op":"release","id":"body"}'
FONT_INFO = base64.b64encode(b'{"id":"body","upem":1000,"glyphs":1,"axes":[],"weight":400,"slant":0.0}')


def tablet(phase, tool, x, buttons=0):
    """One report of a tool, every field of which differs from the last."""
    return osc(b"tablet-pointer", b"v=1:op=event:kind=1:phase=%d:device=3:t=%d:contact=0:x=%d:y=%d:buttons=%d:mods=0:"
                                  b"tool=%d:valid=7:p=%d:tx=%d:ty=-%d" % (phase, 100000 + x, x * 7, x * 3, buttons, tool,
                                                                         x % 65536, x % 9000, x % 9000))


def pen_stroke(tool=1):
    return [tablet(1, tool, 0, 1)] + [tablet(2, tool, x, 1) for x in range(1, POINTS)] + [tablet(3, tool, POINTS)]


def read_reports(until=None, phase=None):
    """The reports of the tablet up to and including the one that is `until`, or the first of `phase`."""
    reports = []
    while True:
        reports.append(read_frame(b"\x1b]777;tablet-pointer;v=1:op=event"))
        if reports[-1] == until or (phase is not None and field(reports[-1], b"phase") == phase):
            return reports


def check_stroke(reports, tool, points):
    """A stroke as the relay sends it: thinned to its ends and bends, in order, on its line."""
    assert field(reports[0], b"phase") == 1 and field(reports[-1], b"phase") == 3, ("a stroke begins and ends", reports[0], reports[-1])
    assert all(field(r, b"tool") == tool for r in reports), "one tool"
    ts = [field(r, b"t") for r in reports]
    assert ts == sorted(ts) and len(set(ts)) == len(ts), ("in order", ts[:5])
    for r in reports:
        x, y = field(r, b"x"), field(r, b"y")
        assert abs(3 * x - 7 * y) <= 7 * 4, ("on the line within a pixel", r)
        assert field(r, b"p") == x % 65536 or field(r, b"phase") != 1, ("pressure of the down", r)
    assert 2 < len(reports) < points // 4, ("thinned", len(reports), points)


def field(report, key):
    return int(re.search(rb"[;:]" + key + rb"=(-?\d+)", report)[1])


def service_and_fonts():
    # Whether the terminal has the service comes just after what it lists.
    deadline = time.monotonic() + 20
    while True:
        os.write(1, b"\x1b_Ga=v,f=200,i=77;" + base64.b64encode(b'{"op":"fonts"}') + b"\x1b\\")
        answer = read_frame(b"\x1b_Gi=77;")
        if answer == b"\x1b_Gi=77;OK;" + SERVICE_ANSWER + b"\x1b\\":
            break
        assert b"EINVAL" in answer and time.monotonic() < deadline, answer
        time.sleep(0.2)
    os.write(1, osc(b"font", b"v=1:op=query:id=8"))
    font = read_frame(b"\x1b]777;font;")
    assert b"id=8" in font and b"roles=default,sans,serif" in font, font
    os.write(1, osc(b"font", b"v=1:op=select:role=serif:weight=300:slant=-500") + b"\x1b[6;1HSERIF_TEXT\x1b[0m"
             + osc(b"font", b"v=1:op=state:id=9"))
    state = read_frame(b"\x1b]777;font;")
    assert b"role=default" in state, state


def session_fonts():
    # An uploaded font, text in it, and its release; then the client repaints.
    os.write(1, b"\x1b_Ga=v,f=200,i=78;" + base64.b64encode(FONT_UPLOAD) + b"\x1b\\")
    answer = read_frame(b"\x1b_Gi=78;")
    assert answer == b"\x1b_Gi=78;OK;" + FONT_INFO + b"\x1b\\", answer
    text = base64.b64encode(b'{"font":"@body","text":"Hi"}')
    os.write(1, b"\x1b_Ga=T,f=200,i=40,s=10,v=10;" + base64.b64encode(b"t id=1 text=" + text) + b"\x1b\\")
    answer = read_frame(b"\x1b_Gi=40;")
    assert answer == b"\x1b_Gi=40;OK\x1b\\", answer
    os.write(1, b"\x1b_Ga=v,f=200,i=79;" + base64.b64encode(FONT_RELEASE) + b"\x1b\\")
    answer = read_frame(b"\x1b_Gi=79;")
    assert answer == b"\x1b_Gi=79;OK;" + base64.b64encode(b'{"id":"body"}') + b"\x1b\\", answer
    os.write(1, b"\x1b[12;1HFONTS_READY")
    deadline = time.monotonic() + PATIENCE
    while b"\x0c" not in INBOX:
        ready, _, _ = select.select([0], [], [], max(0, deadline - time.monotonic()))
        assert ready, ("no Ctrl-L", bytes(INBOX))
        INBOX.extend(os.read(0, 4096))
    del INBOX[:INBOX.find(b"\x0c") + 1]
    os.write(1, b"\x1b[13;1HFONTS_DONE")


def pointer_streams():
    # A pen: every point, unchanged, in order.
    os.write(1, PEN_MODE + b"\x1b[7;1HPEN_READY")
    # A pen: smoothed and thinned, in order, its down and up and what is between kept on its line.
    check_stroke(read_reports(phase=3), 1, POINTS)
    # An eraser is the pen's other end.
    check_stroke(read_reports(phase=3), 2, POINTS)
    os.write(1, b"\x1b[8;1HMOUSE_READY")
    # A mouse: merged, in order, ending where it ended, with the press after it.
    press = tablet(1, 6, POINTS + 1, 1)
    reports = read_reports(press)
    moves = reports[:-1]
    assert 0 < len(moves) < POINTS // 2, ("a mouse was not collected", len(moves))
    xs = [field(m, b"x") for m in moves]
    assert xs == sorted(xs) and xs[-1] == (POINTS - 1) * 7, ("a mouse ends where it ended", xs[-5:])
    assert moves[-1] == tablet(0, 6, POINTS - 1), moves[-1]
    # A pen in the middle of a mouse keeps its stroke: in order, on its line, ending where it ended.
    os.write(1, b"\x1b[9;1HMIXED_READY")
    reports = read_reports(phase=3)
    pen = [r for r in reports if b":tool=1:" in r]
    ts = [field(r, b"t") for r in pen]
    assert ts == sorted(ts) and 1 < len(pen) <= 501, ("a pen among a mouse", len(pen))
    assert field(pen[-1], b"phase") == 3 and field(pen[-1], b"x") == 2 * POINTS * 7, pen[-1]
    assert all(abs(3 * field(r, b"x") - 7 * field(r, b"y")) <= 7 * 4 for r in pen), "on its line"
    # xterm reporting: motion merges, and a press and a release stay.
    os.write(1, XTERM_ON + b"\x1b[?1006h\x1b[10;1HXTERM_READY")
    deadline = time.monotonic() + 20
    while b"\x1b[<0;9;9m" not in INBOX:
        ready, _, _ = select.select([0], [], [], max(0, deadline - time.monotonic()))
        assert ready, ("xterm timeout", bytes(INBOX[-200:]))
        INBOX.extend(os.read(0, 65536))
    keys = bytes(INBOX)
    INBOX.clear()
    motion = re.findall(rb"\x1b\[<35;(\d+);5M", keys)
    assert 0 < len(motion) < POINTS // 2, ("xterm motion was not collected", len(motion))
    assert [int(x) for x in motion] == sorted(int(x) for x in motion) and int(motion[-1]) == POINTS, motion[-5:]
    assert keys.endswith(b"\x1b[<0;9;9M\x1b[<0;9;9m"), keys[-40:]
    os.write(1, b"\x1b[?1003l\x1b[?1006l\x1b[11;1HSTREAMS_OK")


def read_frame(prefix):
    """The next OSC 777 frame the program is sent that starts with prefix."""
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
    service_and_fonts()
    if SESSION_FONTS:
        session_fonts()
    pointer_streams()
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
    env.update(TERM="xterm-256color", MOSH_CLIENT_CAPS="keepalive-v1,sixel-state-v1,link-budget-v1", MOSH_SERVER_NETWORK_TMOUT=str(int(PATIENCE) + 5))
    bootstrap = subprocess.run([server, "new", "-i", "127.0.0.1", "-c", "256", "--", sys.executable, str(SCRIPT),
                                "--remote"], stdin=subprocess.DEVNULL, capture_output=True, env=env, timeout=10)
    assert bootstrap.returncode == 0, bootstrap.stderr
    match = re.search(rb"MOSH CONNECT (\d+) (\S+)", bootstrap.stdout)
    assert match, bootstrap.stdout
    server_pid = int(re.search(rb"detached, pid = (\d+)", bootstrap.stderr)[1])
    relay = module.Relay(int(match[1]), fast=os.environ.get("RELAY") == "fast")
    env.update(MOSH_KEY=match[2].decode(), GOBLIN_SKIFF_SIXEL_STATE="1", GOBLIN_SKIFF_COMPACT_KEEPALIVE="1", GOBLIN_SKIFF_LINK_BUDGET="1",
               GOBLIN_SKIFF_MASCOT="none", MOSH_PREDICTION_DISPLAY="never", MOSH_NO_TERM_INIT="1")
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 80, 640, 512))
    process = subprocess.Popen([client, "127.0.0.1", str(relay.socket.getsockname()[1])], env=env,
                               stdin=slave, stdout=slave, stderr=slave, start_new_session=True,
                               preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
    os.close(slave)
    relay.thread.start()
    output, answered = bytearray(), set()
    service_requests = set()

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
            if KITTY_QUERY in output and "kitty" not in answered:
                os.write(master, b"\x1b_Gi=4294967294;OK\x1b\\")
                answered.add("kitty")
            if b"\x1b[6n" in output and "cpr" not in answered:
                os.write(master, b"\x1b[20;1R")
                answered.add("cpr")
            # A vector service, and it has something to say to all it is asked.
            for match in re.finditer(rb"\x1b_Ga=v,f=200,i=(\d+),m=0;([^\x1b]*)\x1b\\", output):
                if match[1] not in service_requests:
                    service_requests.add(match[1])
                    assert b"q=" not in match[0], match[0]
                    if base64.b64decode(match[2]) == b'{"op":"fonts"}':
                        os.write(master, b"\x1b_Gi=" + match[1] + b";OK;" + SERVICE_ANSWER + b"\x1b\\")
                    elif base64.b64decode(match[2]) == FONT_UPLOAD:
                        os.write(master, b"\x1b_Gi=" + match[1] + b";OK;" + FONT_INFO + b"\x1b\\")
                    else:
                        os.write(master, b"\x1b_Gi=" + match[1] + b";EINVAL:" + SERVICE_ANSWER + b"\x1b\\")

    def feed(data, pieces=256):
        # The terminal writes as it goes, and reads what the client draws meanwhile.
        for at in range(0, len(data), pieces):
            os.write(master, data[at:at + pieces])
            collect(0)

    def until(marker, start=0):
        deadline = time.monotonic() + PATIENCE
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

        until(b"SERIF_TEXT", start)
        assert b"\x1b]777;font;v=1:op=select:role=serif:weight=300:slant=-500\x1b\\SERIF_TEXT" in output[start:], \
            ("the font of the text reaches the terminal", bytes(output[start:])[-300:])

        # The program's upload reaches the terminal as it made it, then the
        # scene, then, quietly, the release, after what may still use it.
        if SESSION_FONTS:
            until(b"FONTS_READY", start)
            replay = b"\x1b_Ga=v,f=200,i=7254,q=2,m=0;"
            until(replay + base64.b64encode(FONT_RELEASE), start)
            fonts = bytes(output[start:])
            scene, release = fonts.find(b"\x1b_Ga=t,q=2,f=200"), fonts.find(replay + base64.b64encode(FONT_RELEASE))
            assert 0 <= fonts.find(base64.b64encode(FONT_UPLOAD)) < scene < release, ("fonts before a repaint", fonts[-600:])
            assert replay + base64.b64encode(FONT_UPLOAD) not in fonts, "the program's own upload is not made again"
            # A repaint gives the terminal the released font the scene's text
            # names, before the scene, and releases it after.
            start = len(output)
            os.write(master, b"\x0c")
            until(b"FONTS_DONE", start)
            until(replay + base64.b64encode(FONT_RELEASE), start)
            repaint = bytes(output[start:])
            upload, scene = repaint.find(replay + base64.b64encode(FONT_UPLOAD)), repaint.find(b"\x1b_Ga=t,q=2,f=200")
            release = repaint.find(replay + base64.b64encode(FONT_RELEASE))
            assert 0 <= upload < scene < release, ("fonts on a repaint", repaint[-600:])

        until(b"PEN_READY", start)
        until(PEN_MODE, start)
        feed(b"".join(pen_stroke()))
        feed(b"".join(pen_stroke(2)))
        until(b"MOUSE_READY", start)
        feed(b"".join(tablet(0, 6, x) for x in range(POINTS)))
        # What is held goes when it is time, without anything more from the mouse.
        collect(0.5)
        feed(tablet(1, 6, POINTS + 1, 1))
        until(b"MIXED_READY", start)
        mixed = bytearray()
        for x in range(1, 501):
            mixed += b"".join(tablet(0, 6, 10000 + 2 * x + k) for k in range(3))
            mixed += tablet(2, 1, 2 * x, 1)
        feed(bytes(mixed) + tablet(3, 1, 2 * POINTS))
        until(b"XTERM_READY", start)
        until(b"\x1b[?1003h", start)
        feed(b"".join(b"\x1b[<35;%d;5M" % x for x in range(1, POINTS + 1)))
        collect(0.5)
        feed(b"\x1b[<0;9;9M\x1b[<0;9;9m")
        until(b"STREAMS_OK", start)

        start = len(output)
        os.write(master, b"q")
        until(b"FINISHED", start)
        until(SCROLL_OFF, start)
        until(b"[goblin-skiff is exiting.]", start)
        process.wait(timeout=5)
        collect(0.1)
        assert process.returncode == 0, ("unclean shutdown", process.returncode, output[-1200:])
        assert osc(b"display", b"v=1:op=mode:mode=0") in output, "the client turns its display reports off"
        print("PASS: capabilities, grants, reports, haptics, HDR SGR, fonts, the vector service, and whole pen strokes "
              "and collected mouse motion through skiff")
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
