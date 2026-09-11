"""Exercise tmux passthrough through two real goblin-mosh endpoints and PTYs."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import tty

ENTRY = b"\x1bP1000p"
EXIT = b"%exit\r\n\x1b\\"
# Exercise raw control characters and more data than either queue limit.
COMMAND = b"send-keys \x1e.\x1bOA\x00 " + b"0123456789abcdef" * 8192 + b"\n"
BODY = b"%output %0 " + b"escaped\\033 unicode:\xc3\xa9 " * 8192 + b"\r\n"


def write_all(fd, data):
    deadline = time.monotonic() + 30
    while data:
        remaining = deadline - time.monotonic()
        assert remaining > 0, "write timeout"
        _, ready, _ = select.select([], [fd], [], remaining)
        assert ready, "write timeout"
        data = data[os.write(fd, data[:1024]):]


def remote():
    tty.setraw(0)
    # Entry and exit split across PTY reads.
    for piece in (b"before\r\n\x1bP", b"100", b"0p%ready\r\n"):
        write_all(1, piece)
        time.sleep(0.05)
    received = b""
    while len(received) < len(COMMAND):
        ready, _, _ = select.select([0], [], [], 20)
        assert ready, "remote command timeout"
        received += os.read(0, 16384)
    assert received == COMMAND, "control commands changed on the wire"
    write_all(1, b"%received\r\n")
    time.sleep(0.4)  # Predictions/redraws must stay out of the protocol.
    write_all(1, BODY + b"%exit\r\n\x1b")
    time.sleep(0.05)
    write_all(1, b"\\after\r\n")
    # Reenter without restarting mosh, then leave abruptly with no ST.
    time.sleep(0.4)
    write_all(1, ENTRY + b"%abrupt\r\n")


class Session:
    def __init__(self, command):
        build = Path.cwd()
        self.master, slave = pty.openpty()
        os.set_blocking(self.master, False)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 800, 480))
        argv = [str(build / "../../scripts/goblin-mosh"),
                "--client=" + str(build / "../frontend/goblin-mosh-client"),
                "--server=" + str(build / "../frontend/goblin-mosh-server"),
                "--tmux-control", "--predict=always", "--local",
                "--bind-server=127.0.0.1", "127.0.0.1", "--", *command]
        env = os.environ.copy()
        env.update(TERM="xterm-256color")
        env.pop("TMUX", None)
        self.proc = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=slave,
                                     env=env, start_new_session=True,
                                     preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
        os.close(slave)
        self.output = b""

    def until(self, marker, timeout=30, after=0):
        deadline = time.monotonic() + timeout
        while marker not in self.output[after:]:
            remaining = deadline - time.monotonic()
            assert remaining > 0, "output timeout: " + repr(self.output[-400:])
            ready, _, _ = select.select([self.master], [], [], remaining)
            assert ready, "output timeout: " + repr(self.output[-400:])
            try:
                data = os.read(self.master, 65536)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                data = b""
            assert data, "early EOF: " + repr(self.output[-400:])
            self.output += data

    def send(self, data):
        try:
            write_all(self.master, data)
        except OSError:
            while select.select([self.master], [], [], 0)[0]:
                try:
                    more = os.read(self.master, 65536)
                except OSError:
                    break
                if not more:
                    break
                self.output += more
            raise AssertionError("input failed (status %r): %r" % (self.proc.poll(), self.output[-1500:]))

    def close(self):
        if self.master is None:
            return
        # Close the master before waiting: Darwin may keep a child in tty
        # teardown until its master is closed, even after SIGKILL.
        os.close(self.master)
        self.master = None
        if self.proc.poll() is None:
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait(timeout=8)

    def finish(self):
        self.until(b"[goblin-mosh is exiting.]")
        self.close()
        assert self.proc.returncode == 0, "unclean mosh shutdown"


def synthetic_test():
    session = Session([sys.executable, str(Path(__file__).resolve()), "--remote"])
    try:
        session.until(ENTRY + b"%ready\r\n")
        session.send(COMMAND)
        session.until(b"%received\r\n")
        session.proc.send_signal(signal.SIGSTOP)
        time.sleep(0.8)
        session.proc.send_signal(signal.SIGCONT)
        session.until(EXIT)
        start = session.output.index(ENTRY)
        end = session.output.index(EXIT, start) + len(EXIT)
        assert session.output[start:end] == ENTRY + b"%ready\r\n%received\r\n" + BODY + EXIT, "protocol bytes changed"
        session.until(b"after")
        session.until(ENTRY + b"%abrupt\r\n\r\n" + EXIT)
        session.finish()
        print("PASS: exact bytes, large queues, pause/resume, detach, reentry, abrupt EOF")
    finally:
        session.close()


def real_tmux_test():
    tmux = shutil.which("tmux")
    if not tmux:
        print("SKIP: real tmux unavailable (synthetic integration passed)")
        return
    with tempfile.TemporaryDirectory(prefix="goblin-tmux-") as directory:
        socket = str(Path(directory) / "socket")
        session = Session([tmux, "-S", socket, "-f", "/dev/null", "-CC",
                           "new-session", "-s", "goblin-test", "sleep 60"])
        try:
            session.until(b"%session-changed")
            session.send(b"refresh-client -C 100,35\ndisplay-message -p 'goblin-control-roundtrip'\n")
            session.until(b"\r\ngoblin-control-roundtrip\r\n")
            fcntl.ioctl(session.master, termios.TIOCSWINSZ, struct.pack("HHHH", 35, 100, 1000, 700))
            session.proc.send_signal(signal.SIGWINCH)
            session.send(b"detach-client\n")
            session.until(b"\x1b\\", after=session.output.index(ENTRY) + len(ENTRY))
            protocol = session.output.split(ENTRY, 1)[1].split(b"\x1b\\", 1)[0]
            assert b"%exit" in protocol, "tmux exit missing"
            assert b"\x1b" not in protocol, "mosh redraw corrupted tmux protocol"
            session.finish()
            print("PASS: real tmux -CC commands, resize, detach")
        finally:
            session.close()
            subprocess.run([tmux, "-S", socket, "kill-server"], check=False,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)


def old_peer_test():
    build = Path.cwd()
    # A bootstrap-only stub, with a dummy key and no UDP server, represents
    # an older endpoint which does not confirm the new capability.
    command = [str(build / "../../scripts/goblin-mosh"),
               "--client=" + str(build / "../frontend/goblin-mosh-client"),
               "--server=printf 'MOSH CONNECT 60000 AAAAAAAAAAAAAAAAAAAAAA\\n'; true",
               "--tmux-control", "--local", "127.0.0.1"]
    env = os.environ.copy()
    env["TERM"] = "xterm"
    result = subprocess.run(command, capture_output=True, env=env, timeout=10)
    assert result.returncode != 0, "old server must not silently downgrade"
    assert b"did not negotiate the requested tmux control mode" in result.stderr, result.stderr
    print("PASS: unsupported server rejected during bootstrap")


if __name__ == "__main__":
    if sys.argv[1:] == ["--remote"]:
        remote()
    else:
        old_peer_test()
        synthetic_test()
        real_tmux_test()
