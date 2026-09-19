"""Live terminal extensions through real PTYs and lossy encrypted UDP."""
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
KEYS = b"\x1bf\x1b[57443;3:1u\x1b[57443;1:3u\x1b[57449;3:1u\x1b[57449;1:3u"
QUERY = b"\x1b_Gi=4294967294,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\"


def read_bytes(expected):
    received = b""
    deadline = time.monotonic() + 20
    while len(received) < len(expected):
        ready, _, _ = select.select([0], [], [], max(0, deadline - time.monotonic()))
        assert ready, "remote input timeout"
        received += os.read(0, len(expected) - len(received))
    assert received == expected, (received, expected)


def remote(graphics):
    tty.setraw(0)
    os.write(1, b"\x1b[c\x1b[?u\x1b[16t")
    read_bytes((b"\x1b[?62;4c" if graphics else b"\x1b[?62c") + b"\x1b[?0u\x1b[6;16;8t")
    image = b'\x1bP0;1q"1;1;5120;96#1;2;100;0;0' + b"-".join([b"!5120~"] * 16) + b"\x1b\\"
    os.write(1, image + b"\x1b[9;1HIMAGE_READY")
    read_bytes(b"r")
    os.write(1, b"\x1b[10;1HRETAINED_IMAGE")
    read_bytes(b"s")
    os.write(1, b"\x1b[H\x1b[2J\x1b]66;s=2:w=3:n=1:d=2;Sized\x1b\\\x1b[>27u\x1b[9;1HTEXT_READY")
    read_bytes(KEYS)
    os.write(1, b"\x1b[10;1HKEYS_OK")
    read_bytes(b"q")
    os.write(1, b"\x1b[<u\x1b[11;1HFINISHED")


def session(kitty, sixel, sizing, escape_exit=False, palette_djvu=True):
    spec = importlib.util.spec_from_file_location("graphics_relay", SCRIPT.with_name("control-panel-integration.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    build = Path.cwd()
    client = str((build / "../frontend/goblin-skiff-client").resolve())
    server = str((build / "../frontend/goblin-skiff-server").resolve())
    env = os.environ.copy()
    env.update(TERM="xterm-256color", MOSH_CLIENT_CAPS="keepalive-v1,sixel-state-v1", MOSH_SERVER_NETWORK_TMOUT="25")
    if palette_djvu:
        env["MOSH_CLIENT_CAPS"] += ",palette-djvu-v1"
    image_options = ["--lossy=75", "--djvu-lossy"] if palette_djvu else []
    bootstrap = subprocess.run([server, "new"] + image_options + ["-i", "127.0.0.1", "-c", "256", "--", sys.executable,
                                str(SCRIPT), "--remote", str(int(kitty or sixel))],
                               stdin=subprocess.DEVNULL, capture_output=True, env=env, timeout=10)
    assert bootstrap.returncode == 0, bootstrap.stderr
    assert b"MOSH GRAPHICS sixel-state-v1" in bootstrap.stdout
    assert (b"MOSH IMAGE palette-djvu-v1" in bootstrap.stdout) == palette_djvu
    match = re.search(rb"MOSH CONNECT (\d+) (\S+)", bootstrap.stdout)
    assert match, bootstrap.stdout
    server_pid = int(re.search(rb"detached, pid = (\d+)", bootstrap.stderr)[1])
    relay = module.Relay(int(match[1]))
    env.update(MOSH_KEY=match[2].decode(), MOSH_SIXEL_STATE="1", MOSH_COMPACT_KEEPALIVE="1",
               MOSH_MASCOT="none", MOSH_PREDICTION_DISPLAY="never", MOSH_NO_TERM_INIT="1")
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 640, 5120, 512))
    process = subprocess.Popen([client, "127.0.0.1", str(relay.socket.getsockname()[1])], env=env,
                               stdin=slave, stdout=slave, stderr=slave, start_new_session=True,
                               preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
    os.close(slave)
    relay.thread.start()
    output, answered = bytearray(), set()
    cpr_count = 0

    def collect(duration=0.1):
        nonlocal cpr_count
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
            responses = ((QUERY, b"\x1b_Gi=4294967294;" + (b"OK" if kitty else b"ENOTSUP") + b"\x1b\\"),
                         (b"\x1b[c", b"\x1b[?62;4c" if sixel else b"\x1b[?62c"),
                         (b"\x1b[?u", b"\x1b[?0u"))
            for query, response in responses:
                if query in output and query not in answered:
                    os.write(master, response)
                    answered.add(query)
            while cpr_count < min(3, output.count(b"\x1b[6n")):
                col = (1, 3, 5)[cpr_count] if sizing else 1
                os.write(master, f"\x1b[20;{col}R".encode())
                cpr_count += 1

    def until(marker, start=0):
        deadline = time.monotonic() + 25
        while marker not in output[start:]:
            assert time.monotonic() < deadline, ("missing", marker, bytes(output[-1500:]))
            assert process.poll() is None, (process.returncode, bytes(output[-1500:]))
            collect()

    try:
        until(b"IMAGE_READY")
        collect(0.2)
        # Query packets are not image transmissions. Native sixel wins on
        # dual-capable terminals; conversion occurs only at the local client.
        assert (b"\x1bP" in output) == sixel
        assert (b"\x1b_Ga=t" in output) == (kitty and not sixel), bytes(output[:300])
        if kitty and not sixel:
            assert b"s=5120,v=96" in output, "wide image was downscaled or truncated"
        start = len(output)
        os.write(master, b"r")
        until(b"RETAINED_IMAGE", start)
        collect(0.2)
        assert b"\x1bP" not in output[start:] and b"\x1b_Ga=t" not in output[start:], "unchanged image retransmitted locally"
        start = len(output)
        os.write(master, b"s")
        until(b"TEXT_READY", start)
        until(b"\x1b[=27u", start)
        collect(0.1)
        assert (b"\x1b]66;s=2:w=3:n=1:d=2;Sized\x1b\\" in output[start:]) == sizing
        assert b"Sized" in output[start:], "plain fallback lost sized text"
        os.write(master, KEYS)
        until(b"KEYS_OK", start)
        os.write(master, b"\x1b[54;5:1u\x1b[54;5:3u\x1b[46u" if escape_exit else b"q")
        until(b"[goblin-skiff is exiting.]", start)
        process.wait(timeout=5)
        collect(0.1)
        assert process.returncode == 0, ("unclean shutdown", process.returncode, output[-1200:])
        assert b"\x1b[<u" in output, "keyboard mode not restored at exit"
        assert relay.dropped > 0, "test did not exercise packet loss"
        print(f"PASS: kitty={kitty} sixel={sixel} OSC66={sizing}; 5120px, retained image, Alt events, lossy UDP, clean exit")
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
        remote(bool(int(sys.argv[2])))
    else:
        for capabilities in ((True, False, True), (False, True, False), (True, True, True), (False, False, False)):
            session(*capabilities)
        session(True, False, True, escape_exit=True, palette_djvu=False)
