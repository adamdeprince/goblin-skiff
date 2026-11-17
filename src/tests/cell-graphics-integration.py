"""A complete cell-tiled desktop through real PTYs and encrypted UDP."""
import base64
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import struct
import subprocess
import sys
import termios
import time
import tty

ROWS, COLS, WIDTH, HEIGHT = 77, 126, 14, 25
COUNT = ROWS * COLS
# A whole 9,702-tile screen takes longer on slower native build hosts.
PATIENCE = float(os.environ.get("PATIENCE", "300"))
SCRIPT = Path(__file__).resolve()


def color(image_id):
    return bytes((image_id % 241, 83, 149))


def tile(image_id, rgb):
    row, col = divmod(image_id - 1, COLS)
    return (f"\x1b[{row+1};{col+1}H\x1b_Ga=T,t=d,f=24,s={WIDTH},v={HEIGHT},"
            f"i={image_id},p=1,c=1,r=1,C=1,q=2,m=0;".encode()
            + base64.b64encode(rgb * (WIDTH * HEIGHT)) + b"\x1b\\")


def remote():
    tty.setraw(0)
    sys.stdout.buffer.write(b"\x1b[?1049h\x1b[2J\x1b[H"
                            + b"".join(tile(i, color(i)) for i in range(1, COUNT + 1)))
    sys.stdout.buffer.flush()
    assert os.read(0, 1) == b"u"
    sys.stdout.buffer.write(tile(COUNT, b"\x11\x22\x33"))
    sys.stdout.buffer.flush()
    assert os.read(0, 1) == b"q"


def session():
    build = Path.cwd()
    client = os.environ.get("GOBLIN_SKIFF_TEST_CLIENT", str((build / "../frontend/goblin-skiff-client").resolve()))
    server = os.environ.get("GOBLIN_SKIFF_TEST_SERVER", str((build / "../frontend/goblin-skiff-server").resolve()))
    wrapper = os.environ.get("GOBLIN_SKIFF_TEST_WRAPPER", str((build / "../../scripts/goblin-skiff").resolve()))
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, COLS * WIDTH, ROWS * HEIGHT))
    env = dict(os.environ, TERM="xterm-256color", MOSH_SERVER_NETWORK_TMOUT=str(int(PATIENCE) + 15))
    args = [wrapper, "--client=" + client, "--server=" + server, "--no-mascot", "--predict=never",
            "--local", "--bind-server=127.0.0.1", "--", "127.0.0.1", sys.executable, str(SCRIPT), "--remote"]
    proc = subprocess.Popen(args, stdin=slave, stdout=slave, stderr=slave, env=env,
                            start_new_session=True, preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
    os.close(slave)
    os.set_blocking(master, False)
    pending_input = bytearray()
    pending = b""
    images, placements = {}, {}
    cursor = (0, 0)
    upload = None
    started = time.monotonic()
    phase = 0
    deadline = started + PATIENCE
    pattern = rb"\x1b(?:_G[^\x1b]*\x1b\\|\][^\x1b]*(?:\x1b\\|\x07)|\[[0-?]*[ -/]*[@-~])"
    try:
        while proc.poll() is None:
            assert time.monotonic() < deadline, ("cell desktop timeout", phase, len(images), len(placements))
            readable, writable, _ = select.select([master], [master] if pending_input else [], [], .05)
            if writable:
                try:
                    count = os.write(master, pending_input)
                    del pending_input[:count]
                except BlockingIOError:
                    pass
            if not readable:
                continue
            try:
                pending += os.read(master, 65536)
            except OSError as error:
                if error.errno == errno.EAGAIN:
                    continue
                if error.errno == errno.EIO:
                    break
                raise
            while True:
                match = re.search(pattern, pending)
                if not match:
                    start = pending.find(b"\x1b")
                    pending = pending[start:] if start >= 0 else b""
                    break
                frame = match[0]
                pending = pending[match.end():]
                if frame.startswith(b"\x1b_G"):
                    header, _, payload = frame[3:-2].partition(b";")
                    fields = dict(part.split(b"=", 1) for part in header.split(b",") if part)
                    action = fields.get(b"a")
                    if action == b"q":
                        pending_input.extend(b"\x1b_Gi=" + fields[b"i"] + b";OK\x1b\\")
                    elif action == b"d":
                        image_id = int(fields[b"i"])
                        placements.pop(image_id, None)
                        if fields[b"d"] == b"I":
                            images.pop(image_id, None)
                    elif action == b"p":
                        image_id = int(fields[b"i"])
                        assert image_id in images and fields[b"p"] == fields[b"c"] == fields[b"r"] == b"1"
                        assert cursor == divmod(image_id - 1, COLS), (image_id, cursor)
                        placements[image_id] = cursor
                    elif action == b"t":
                        assert (fields[b"f"], int(fields[b"s"]), int(fields[b"v"])) == (b"32", WIDTH, HEIGHT)
                        upload = (int(fields[b"i"]), bytearray())
                    if upload is not None and b"m" in fields:
                        upload[1].extend(payload)
                        if fields[b"m"] == b"0":
                            image_id, encoded = upload
                            pixels = base64.b64decode(encoded, validate=True)
                            rgb = b"\x11\x22\x33" if phase and image_id == COUNT else color(image_id)
                            assert pixels == (rgb + b"\xff") * (WIDTH * HEIGHT), ("wrong tile pixels", image_id)
                            images[image_id] = rgb
                            upload = None
                else:
                    cup = re.fullmatch(rb"\x1b\[(\d+);(\d+)H", frame)
                    if cup:
                        cursor = tuple(int(value) - 1 for value in cup.groups())
                    responses = {b"\x1b[c": b"\x1b[?62c", b"\x1b[?u": b"\x1b[?0u",
                                 b"\x1b[6n": b"\x1b[1;1R",
                                 b"\x1b[16t": f"\x1b[6;{HEIGHT};{WIDTH}t".encode(),
                                 b"\x1b[18t": f"\x1b[8;{ROWS};{COLS}t".encode(),
                                 b"\x1b[14t": f"\x1b[4;{ROWS*HEIGHT};{COLS*WIDTH}t".encode()}
                    if frame in responses:
                        pending_input.extend(responses[frame])
            if phase == 0 and len(images) == COUNT and len(placements) == COUNT:
                print(f"All {COUNT} cell images and placements verified in {time.monotonic()-started:.2f}s", flush=True)
                phase = 1
                deadline = time.monotonic() + PATIENCE
                pending_input.extend(b"u")
            elif phase == 1 and images.get(COUNT) == b"\x11\x22\x33" and COUNT in placements:
                phase = 2
                deadline = time.monotonic() + PATIENCE
                pending_input.extend(b"q")
        assert phase == 2, ("desktop did not finish", phase, len(images), pending[-200:])
        assert proc.wait(timeout=5) == 0
        print("Keyboard input, changed tile, and clean shutdown verified")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)
        os.close(master)


if __name__ == "__main__":
    remote() if sys.argv[1:] == ["--remote"] else session()
