"""Real PTYs + encrypted UDP, with loss/reordering and a stalled NAS surrogate."""
import errno
import fcntl
import heapq
import os
from pathlib import Path
import pty
import re
import random
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import tty


def remote():
    tty.setraw(0)
    os.write(1, b"\x1b[1;1HINITIALREADY")
    if os.environ.get("GOBLIN_PANEL_LINK_TEST") == "1":
        generator = random.Random(29)
        # More than one MTU after compression: exercise paced fragments,
        # retransmission and ACKs while the displayed state keeps changing.
        for row in range(2, 23):
            os.write(1, ("\x1b[%d;1H" % row).encode() + bytes(generator.randrange(33, 127) for _ in range(79)))
        os.write(1, b"\x1b[23;1HLARGESTATEREADY")
    counter = 0
    while True:
        readable, _, _ = select.select([0], [], [], 0.1)
        if readable:
            data = os.read(0, 4096)
            with open(os.environ["GOBLIN_PANEL_INPUT_LOG"], "ab") as log:
                log.write(data)
            if b"q" in data:
                return
            if b"z" in data:
                os.write(1, b"\x1b[1;1HKEYBOARDROUNDTRIP")
        counter += 1
        os.write(1, b"\x1b[24;1H" + bytes([65 + counter % 26]) * 24)


class Relay:
    def __init__(self, port, fast=False):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.bind(("127.0.0.1", 0))
        self.server = ("127.0.0.1", port)
        self.client = None
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.dropped = 0
        self.loss = not fast
        self.fast = fast
        self.max_packet = [0, 0]

    def run(self):
        pending = []
        count = 0
        while not self.stopped.is_set():
            readable, _, _ = select.select([self.socket], [], [], 0.01)
            if readable:
                data, origin = self.socket.recvfrom(65535)
                side = int(origin == self.server)
                self.max_packet[side] = max(self.max_packet[side], len(data))
                if origin == self.server:
                    destination = self.client
                else:
                    self.client = origin
                    destination = self.server
                count += 1
                if self.loss and count % 7 == 0:
                    self.dropped += 1
                elif destination:
                    # Delay every fifth packet enough to reorder it.
                    delay = 0 if self.fast else 0.23 if count % 5 == 0 else 0.04
                    heapq.heappush(pending, (time.monotonic() + delay, count, destination, data))
            while pending and pending[0][0] <= time.monotonic():
                _, _, destination, data = heapq.heappop(pending)
                self.socket.sendto(data, destination)

    def close(self):
        self.stopped.set()
        self.thread.join(timeout=2)
        self.socket.close()


def children(pid):
    output = subprocess.check_output(["ps", "-axo", "pid=,ppid="], text=True)
    return {int(child) for child, parent in map(str.split, output.splitlines()) if int(parent) == pid}


def integration(prefix="\x1e", adaptive=False, file_sync=False, speed=False, confirm=False, udp_hops=0, socks5=False):
    build = Path.cwd()
    client = os.environ.get("GOBLIN_TEST_CLIENT", str((build / "../frontend/goblin-skiff-client").resolve()))
    server = os.environ.get("GOBLIN_TEST_SERVER", str((build / "../frontend/goblin-skiff-server").resolve()))
    # Keep session socket paths below sockaddr_un.sun_path on macOS too.
    with tempfile.TemporaryDirectory(prefix="goblin-panel-", dir="/tmp") as root:
        directory = Path(root)
        runtime = directory / "runtime"
        runtime.mkdir(mode=0o700)
        input_log = directory / "input.log"
        input_log.touch()
        local = directory / "local"
        local.mkdir()
        (local / "local-only").touch()
        for index in range(256):
            (directory / ("remote-%04d" % index)).touch()
        if file_sync:
            generator = random.Random(47)
            basis = bytes(generator.randrange(256) for _ in range(32768))
            changed = basis[:16000] + b"changed!" + basis[16008:]
            for parent in (local, directory):
                (parent / "a-sync/nested/empty").mkdir(parents=True)
            (local / "a-sync/nested/data.bin").write_bytes(changed)
            (directory / "a-sync/nested/data.bin").write_bytes(basis)
            (local / "a-sync/new.txt").write_bytes(b"new file\n")
            if speed:
                speed_data = generator.randbytes(512 * 1024)
                (local / "a-sync/speed.bin").write_bytes(speed_data)
        env = os.environ.copy()
        env.update(TERM="xterm-256color", MOSH_CLIENT_CAPS="keepalive-v1,directory-v1,directory-v2",
                   MOSH_SERVER_NETWORK_TMOUT="20", MOSH_ESCAPE_KEY=prefix,
                   XDG_RUNTIME_DIR=str(runtime), GOBLIN_PANEL_INPUT_LOG=str(input_log))
        if adaptive:
            env["MOSH_CLIENT_CAPS"] += ",link-budget-v1"
            if not file_sync:
                env["GOBLIN_PANEL_LINK_TEST"] = "1"
        if file_sync:
            env["MOSH_CLIENT_CAPS"] += ",file-sync-v1"
        if udp_hops:
            env["MOSH_RELAY_HOPS"] = str(udp_hops)
            env["MOSH_CLIENT_CAPS"] += ",udp-relay-v1"
        bootstrap = subprocess.run([server, "new", "-i", "127.0.0.1", "-c", "256", "--",
                                    sys.executable, str(Path(__file__).resolve()), "--remote"],
                                   stdin=subprocess.DEVNULL, capture_output=True, cwd=root, env=env, timeout=10)
        assert bootstrap.returncode == 0, bootstrap.stderr
        assert b"MOSH DIRECTORY directory-v2" in bootstrap.stdout
        assert (b"MOSH LINK budget-v1" in bootstrap.stdout) == adaptive
        match = re.search(rb"MOSH CONNECT (\d+) (\S+)", bootstrap.stdout)
        assert match, "bootstrap missing connection parameters"
        server_pid = int(re.search(rb"detached, pid = (\d+)", bootstrap.stderr)[1])
        if file_sync and b"MOSH FILES file-sync-v1" not in bootstrap.stdout:
            os.kill(server_pid, signal.SIGTERM)
            print("SKIP: file-menu synchronization not enabled in this build")
            raise SystemExit(77)
        jump_processes, jump_keys = [], []
        port = int(match[1])
        if udp_hops:
            assert ("MOSH RELAY-MTU 1 %d" % udp_hops).encode() in bootstrap.stdout
            for _ in range(udp_hops):
                jump = subprocess.Popen([server, "relay", "--foreground", "--bind=127.0.0.1", "--port=0",
                                         "--startup-timeout=10", "--idle-timeout=20", "127.0.0.1", str(port)],
                                        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
                jump_processes.append(jump)
                assert select.select([jump.stdout], [], [], 5)[0], "relay bootstrap timed out"
                banner = jump.stdout.readline().split()
                assert banner[:4] == [b"MOSH", b"RELAY", b"1", b"ocb-aes128"], banner[:4]
                port = int(banner[5])
                jump_keys.insert(0, banner[6].decode())
            env["MOSH_RELAY_KEYS"] = ",".join(jump_keys)
        relay = Relay(port, fast=speed)
        proxy = None
        proxy_args = []
        if socks5:
            import runpy
            Proxy = runpy.run_path(str(Path(__file__).with_name("socks5-integration.py")))["Proxy"]
            # Relay supplies the established one-in-seven loss/reordering
            # workload. Malformed-packet flooding is tested independently.
            proxy = Proxy(restart=True, loss=False, noisy=False)
            proxy_args = ["--socks5-proxy=" + proxy.endpoint]
        env.update(MOSH_KEY=match[2].decode(), MOSH_DIRECTORY="2", MOSH_COMPACT_KEEPALIVE="1",
                   MOSH_FILES="1" if file_sync else "0",
                   MOSH_LINK_BUDGET="1" if adaptive else "0",
                   MOSH_MASCOT="none", MOSH_PREDICTION_DISPLAY="never", MOSH_NO_TERM_INIT="1")
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 800, 480))
        proc = subprocess.Popen([client] + (["--udp-relay"] if udp_hops else []) + proxy_args
                                + ["only-in-tailnet.invalid" if socks5 else "127.0.0.1", str(relay.socket.getsockname()[1])],
                                cwd=local, env=env, stdin=slave, stdout=slave, stderr=slave,
                                start_new_session=True,
                                preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
        os.close(slave)
        relay.thread.start()
        output = bytearray()
        stopped = set()

        def collect(duration):
            deadline = time.monotonic() + duration
            while time.monotonic() < deadline:
                readable, _, _ = select.select([master], [], [], max(0, deadline - time.monotonic()))
                if readable:
                    try:
                        data = os.read(master, 65536)
                    except OSError as error:
                        if error.errno != errno.EIO:
                            raise
                        data = b""
                    if not data:
                        return
                    output.extend(data)

        def until(marker, start=0, timeout=30):
            deadline = time.monotonic() + timeout
            while marker not in output[start:]:
                assert time.monotonic() < deadline, "missing %r: first %r; last %r" % (marker, output[start:start + 1800], output[-500:])
                collect(0.1)

        def expect_input(wanted):
            deadline = time.monotonic() + 15
            while len(input_log.read_bytes()) < len(wanted):
                assert time.monotonic() < deadline, ("input missing", wanted, input_log.read_bytes(), output[-800:])
                collect(0.1)
            assert input_log.read_bytes() == wanted, (wanted, input_log.read_bytes())

        def send_through(wire, expected):
            previous = input_log.read_bytes()
            # A trailing ordinary key confirms that a close command and the
            # rest of the same input read are routed to the remote application.
            os.write(master, wire + b"z")
            expect_input(previous + expected + b"z")

        def toggle_pair(shortcut):
            previous = input_log.read_bytes()
            start = len(output)
            os.write(master, shortcut)
            until(b"Files", start)
            collect(0.2)
            assert input_log.read_bytes() == previous, "local command leaked to remote application"
            send_through(shortcut, b"")

        try:
            until(b"INITIALREADY")
            if file_sync:
                os.write(master, b"\x1e0")
                until(b"local-only")
                until(b"remote-")
                until(b"64 cached")
                start = len(output)
                os.write(master, b"\x1b[200~\r\x1b[201~")
                collect(0.3)
                assert (directory / "a-sync/nested/data.bin").read_bytes() == basis
                began = time.monotonic()
                # --file-speed-confirm measures older installed builds fairly.
                os.write(master, b"\x1b[15~" if confirm else b"\r")
                if confirm:
                    until(b"synchronize", start)
                    began = time.monotonic()
                    os.write(master, b"\r")
                until(b"Transf", start)  # Terminal diffs may reuse the following 'e'.
                if not confirm:
                    assert b"synchronize" not in output[start:], "copy replaced browser with confirmation"
                send_through(b"\x1e0", b"")  # Underlying keyboard stays live during copy.

                def copied(path, expected):
                    deadline = time.monotonic() + (300 if speed else 60)
                    while not path.exists() or path.read_bytes() != expected:
                        assert proc.poll() is None, output[-1200:]
                        if time.monotonic() >= deadline:
                            os.write(master, b"\x1e0")
                            collect(0.3)
                            raise AssertionError(("file copy timeout", path, output[-4000:]))
                        collect(0.1)

                copied(directory / "a-sync/new.txt", b"new file\n")
                copied(directory / "a-sync/nested/data.bin", changed)
                if speed:
                    copied(directory / "a-sync/speed.bin", speed_data)
                    duration = time.monotonic() - began
                    print("FILE SPEED upload: %d bytes in %.3f s = %.3f Mbit/s (encrypted loopback, cold start, incompressible)"
                          % (len(speed_data), duration, len(speed_data) * 8 / duration / 1e6), flush=True)
                collect(2)
                start = len(output)
                os.write(master, b"\x1e0")
                until(b"Transfers", start)  # Jobs survive hiding and reopening.
                replacement = changed[:24000] + b"remote!!" + changed[24008:]
                (directory / "a-sync/nested/data.bin").write_bytes(replacement)
                if speed:
                    (directory / "a-sync/speed-download.bin").write_bytes(speed_data)
                os.write(master, b"\x1b[Z")  # Shift-Tab switches to the remote pane too.
                collect(0.2)
                start = len(output)
                began = time.monotonic()
                os.write(master, b"\x1b[15~" if confirm else b"\x1b[13;193:1u\x1b[13;193:3u")
                if confirm:
                    until(b"synchronize", start)
                    began = time.monotonic()
                    os.write(master, b"\x1b[13u")
                copied(local / "a-sync/nested/data.bin", replacement)
                if speed:
                    copied(local / "a-sync/speed-download.bin", speed_data)
                    duration = time.monotonic() - began
                    print("FILE SPEED download: %d bytes in %.3f s = %.3f Mbit/s (encrypted loopback, incompressible)"
                          % (len(speed_data), duration, len(speed_data) * 8 / duration / 1e6), flush=True)
                assert (local / "a-sync/nested/empty").is_dir()
                assert speed or relay.dropped > 0
                # All transfer assertions above run with loss. Mosh's existing
                # one-shot final shutdown ACK can itself be dropped after the
                # server exits; keep teardown deterministic, not a test of that
                # separate shutdown limitation.
                relay.loss = False
                collect(0.3)
                os.write(master, b"\x1e.")
                until(b"[goblin-skiff is exiting.]")
                proc.wait(timeout=5)
                collect(0.2)
                assert proc.returncode == 0, ("file-menu shutdown", proc.returncode, output[-3000:])
                for jump in jump_processes:
                    assert jump.wait(timeout=5) == 0, "relay did not close after client exit"
                if udp_hops:
                    # Adaptive pacing may deliberately use smaller datagrams.
                    # Full-MTU framing is exercised separately by udp-relay.
                    assert all(0 < size <= 1216 for size in relay.max_packet), ("relay MTU", relay.max_packet)
                print("PASS: file-menu direct Enter/Shift-Tab/Kitty keys, paste safety, recursive delta upload/download, encrypted UDP, live keyboard and persistent queue", flush=True)
                return
            if adaptive:
                until(b"LARGESTATEREADY")
            session_children = children(server_pid)
            send_through(b"\x1b0\x1b[48;3u", b"\x1b0\x1b[48;3u")
            pasted = b"\x1b[200~\x1e0\x1b[201~"
            send_through(pasted, pasted)
            if prefix == "\x1e":
                send_through(b"\x1e^\x1ex", b"\x1e\x1ex")
                toggle_pair(b"\x1e0")
                # Press-only CSI-u must work repeatedly without key releases.
                toggle_pair(b"\x1b[54;5u\x1b[48u")  # Kitty Ctrl-6.
                toggle_pair(b"\x1b[54;6u\x1b[48u")
                toggle_pair(b"\x1b[54::54;6u\x1b[48::48u")  # Alternate keys.
                toggle_pair(b"\x1b[54:94:54;6;54u\x1b[48;;48u")  # Associated text, default modifiers.
                zero_events = b"\x1b[48;1:1u\x1b[48;1:2u\x1b[48;1:3u"
                send_through(zero_events, zero_events)
                toggle_pair(b"\x1b[54;198u\x1b[48;193u")  # Caps/Num Lock.
                toggle_pair(b"\x1b[54;6:1u\x1b[54;6:2u\x1b[54;1:3u"
                            b"\x1b[48;1:1u\x1b[48;1:2u\x1b[48;1:3u")
                print("PASS: legacy/Kitty prefixes, repeats/releases, lock modifiers, paste, literal/unknown commands, Alt-0 passthrough", flush=True)
            elif prefix == "\x02":
                send_through(b"\x1e0\x02b\x02x", b"\x1e0\x02\x02x")
                toggle_pair(b"\x020")
                toggle_pair(b"\x1b[98;5u\x1b[48u")
                assert b"Ctrl-B 0" in output, "configured prefix missing from panel help"
            elif prefix == "~":
                send_through(b"x~0", b"x~0")
                start = len(output)
                previous = input_log.read_bytes()
                os.write(master, b"\r~0")
                until(b"Files", start)
                expect_input(previous + b"\r")
                send_through(b"\r~0", b"")  # Enter is handled by the visible pane.
                assert b'Enter "~" 0' in output, "printable prefix missing from panel help"
                send_through(b"\r~~", b"\r~")
            else:
                assert prefix == ""
                send_through(b"\x1e0", b"\x1e0")
            if prefix != "\x1e":
                relay.loss = False  # Do not drop the server's one-shot final shutdown ACK.
                collect(0.3)
                os.write(master, b"q")
                until(b"[goblin-skiff is exiting.]")
                proc.wait(timeout=5)
                assert proc.returncode == 0
                print("PASS: configured Mosh prefix %r" % prefix, flush=True)
                return
            os.write(master, b"\x1e0")
            until(b"local-only")
            until(b"remote-")
            until(b"64 cached")
            assert b"Page 1" not in output, "transport pagination leaked into the UI"
            # Updates are generated by both endpoints, not by pressing r.
            (local / "added-live").write_bytes(b"x" * 12345)
            until(b"added-live")
            (local / "added-live").unlink()
            collect(1)
            helpers = children(server_pid) - session_children
            assert len(helpers) == 1, "exactly one remote filesystem worker"
            for pid in helpers:
                os.kill(pid, signal.SIGSTOP)
                stopped.add(pid)
            start = len(output)
            os.write(master, b"\t" + b"j" * 64)  # Scroll across a chunk boundary; helper cannot answer.
            until(b"Loading", start)
            collect(2)
            pulses = re.findall(rb"([A-Z])\1{15}", output[start:])
            if socks5:
                # This added reconnect + adaptive-pacing case tests liveness,
                # not a guaranteed frame rate. Two real updates in the first
                # two seconds are not a frozen screen. Still require three
                # distinct updates within a bounded five-second observation.
                deadline = time.monotonic() + 3
                while len(set(pulses)) < 3 and time.monotonic() < deadline:
                    collect(0.1)
                    pulses = re.findall(rb"([A-Z])\1{15}", output[start:])
            assert len(set(pulses)) >= 3, ("remote screen froze under popup", pulses, output[start:][-4000:])
            assert helpers == children(server_pid) - session_children, "stalled query spawned more workers"
            send_through(b"\x1e0", b"")
            start = len(output)
            os.write(master, b"\x1e0")
            until(b"Loading", start)  # Same pending query, not a fresh listing.
            assert helpers == children(server_pid) - session_children, "reopening restarted directory query"
            for pid in list(stopped):
                os.kill(pid, signal.SIGCONT)
                stopped.remove(pid)
            # Display diff may output just the changed cache count; use a
            # hide/reopen to redraw its complete, persistent status line.
            collect(4)
            os.write(master, b"\x1e0")
            collect(0.2)
            start = len(output)
            os.write(master, b"\x1e0")
            until(b"128 cached", start)
            os.write(master, b"\x1e0")
            collect(0.2)
            os.write(master, b"\x1e0")
            collect(0.2)
            # The normal Mosh quit command must also work inside the popup.
            # All lossy interaction assertions are complete. As in --files,
            # avoid testing the separate one-shot final-ACK limitation here.
            relay.loss = False
            collect(0.3)
            os.write(master, b"\x1e.")
            until(b"[goblin-skiff is exiting.]")
            proc.wait(timeout=5)
            collect(0.2)
            os.close(master)
            master = None
            assert proc.returncode == 0, ("unclean shutdown", proc.returncode, output[-1200:])
            assert relay.dropped > 0
            print("PASS: real UDP loss/reordering, stalled remote worker, live underlying screen, keyboard, persistent reopen")
        finally:
            for pid in stopped:
                try:
                    os.kill(pid, signal.SIGCONT)
                except ProcessLookupError:
                    pass
            if master is not None:
                os.close(master)
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait(timeout=5)
            if proxy:
                proxy.close()
                assert proxy.associations >= 2, "session did not exercise SOCKS5 reconnection"
            relay.close()
            for jump in jump_processes:
                if jump.poll() is None:
                    jump.terminate()
                jump.wait(timeout=5)
                jump.stdout.close()
            try:
                os.kill(server_pid, signal.SIGTERM)
            except ProcessLookupError:
                pass


if __name__ == "__main__":
    if sys.argv[1:] == ["--remote"]:
        remote()
    elif sys.argv[1:] == ["--adaptive"]:
        integration("\x02", adaptive=True)
    elif sys.argv[1:] == ["--files"]:
        integration(file_sync=True, adaptive=True)
    elif sys.argv[1:] == ["--udp-relay"]:
        integration("\x02", adaptive=True, udp_hops=1)
        integration(file_sync=True, adaptive=True, udp_hops=4)
    elif sys.argv[1:] == ["--socks5"]:
        integration(adaptive=True, udp_hops=1, socks5=True)
    elif sys.argv[1:] in (["--file-speed"], ["--file-speed-confirm"]):
        integration(file_sync=True, adaptive=True, speed=True, confirm=sys.argv[1].endswith("-confirm"))
    else:
        for prefix in ("\x1e", "\x02", "~", ""):
            integration(prefix)
