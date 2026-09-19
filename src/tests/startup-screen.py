"""Real PTY checks for inline startup output and local graphics overrides."""
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import tty

BUILD = Path.cwd()
SCRIPT = Path(__file__).resolve()
CLIENT = os.environ.get("GOBLIN_SKIFF_TEST_CLIENT", str((BUILD / "../frontend/goblin-skiff-client").resolve()))
SERVER = os.environ.get("GOBLIN_SKIFF_TEST_SERVER", str((BUILD / "../frontend/goblin-skiff-server").resolve()))
WRAPPER = os.environ.get("GOBLIN_SKIFF_TEST_WRAPPER", str((BUILD / "../../scripts/goblin-skiff").resolve()))
KITTY_QUERY = b"\x1b_Gi=4294967294,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\"
READY = b"remote-startup-ready"
SOURCE_NOTICE = b"[Goblin Skiff GPLv3+ | https://github.com/adamdeprince/goblin-skiff]\r\n"
PANEL_NOTICE = b"Control panel: Ctrl-^ then 0 (Kitty: Ctrl-6, release, then 0).\r\n"


def command(options, remote_command):
    return [WRAPPER,
            "--client=" + CLIENT,
            "--server=" + SERVER,
            "--predict=never", *options, "--local", "--bind-server=127.0.0.1",
            "127.0.0.1", "--", *remote_command]


def remote(graphics=False):
    tty.setraw(0)
    # This image uses normal synchronized terminal state, not the mascot.
    if graphics:
        os.write(1, b"\x1b_Ga=T,C=1,q=2,f=24,s=1,v=1,i=73;/wAA\x1b\\")
    os.write(1, READY + b"\r\n")
    ready, _, _ = select.select([0], [], [], 15)
    assert ready and os.read(0, 4096) == b"q", "local probe reply leaked to the remote PTY"
    os.write(1, b"remote-startup-done\r\n")


def session(options, kitty, sixel, probes, graphics=False, escape_key=None, notice=PANEL_NOTICE, show_mascot=True,
            terminal=None):
    master, slave = pty.openpty()
    rows, columns = (terminal.rows, terminal.columns) if terminal else (24, 80)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, columns * 10, rows * 20))
    env = os.environ.copy()
    env.update(TERM="xterm-256color", GOBLIN_SKIFF_MASCOT="auto")
    env.pop("TMUX", None)
    env.pop("MOSH_ESCAPE_KEY", None)
    if escape_key is not None:
        env["MOSH_ESCAPE_KEY"] = escape_key
    python = os.environ.get("GOBLIN_SKIFF_TEST_PYTHON", sys.executable)
    argv = command(options, [python, str(SCRIPT), "--remote-graphics" if graphics else "--remote"])
    proc = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=slave,
                            env=env, start_new_session=True,
                            preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
    os.close(slave)
    output = b""
    answered = set()
    sent_key = False
    deadline = time.monotonic() + 25
    try:
        while True:
            remaining = deadline - time.monotonic()
            assert remaining > 0, "startup timeout: " + repr(output[-300:])
            ready, _, _ = select.select([master], [], [], min(remaining, 1))
            if not ready:
                assert proc.poll() is None, "client exited without PTY EOF"
                continue
            try:
                data = os.read(master, 65536)
            except OSError as error:
                if error.errno != errno.EIO:
                    raise
                data = b""
            if not data:
                break
            output += data
            if terminal:
                response = terminal.feed(data)
                if response:
                    os.write(master, response)
            else:
                for query, response in ((KITTY_QUERY, b"\x1b_Gi=4294967294;OK\x1b\\"),
                                        (b"\x1b[c", b"\x1b[?62;4c")):
                    if query in output and query not in answered:
                        os.write(master, response)
                        answered.add(query)
            if READY in output and not sent_key:
                if terminal:
                    terminal.check_layout(show_mascot)
                os.write(master, b"q")
                sent_key = True
        assert proc.wait(timeout=5) == 0, repr(output[-500:])
        assert b"remote-startup-done" in output, repr(output[-500:])
        assert output.count(SOURCE_NOTICE) == 1, "source/license notice missing or repeated"
        assert output.count(notice) == 1, "control-panel hint missing, incorrect, or repeated"
        assert output.index(SOURCE_NOTICE) < output.index(notice) < output.index(READY), "panel hint was not part of startup"
        if notice != PANEL_NOTICE:
            assert PANEL_NOTICE not in output, "default shortcut advertised with a different escape prefix"
        assert (b"\x1b_G" in output) == kitty, "Kitty output override: " + repr(options)
        assert (b"\x1bP0;0;0q" in output) == sixel, "sixel output override: " + repr(options)
        assert (KITTY_QUERY in output, b"\x1b[c" in output) == probes, "disabled probe was sent"
        assert b"\x1b[2J" not in output and b"\x1b[3J" not in output, "startup erased screen/history"
        assert output.count(b"Goblin Skiff\r\n") == int(show_mascot), "mascot banner was repeated or missing"
        banner = output.index(b"Goblin Skiff\r\n") if show_mascot else output.index(notice)
        # Attachment metadata may produce an empty first frame while the
        # login child waits for capability discovery. Incremental allocation
        # must reserve the same total number of rows, not necessarily all in
        # one write. Ignore the extra final row printed after q.
        startup_output = output[:output.index(READY)]
        if not terminal:
            allocations = list(re.finditer(rb"\x1b\[24;1H\r(\n+)\x1b\[", startup_output))
            assert sum(len(match[1]) for match in allocations) == (24 if graphics else 2), repr(startup_output[-500:])
            handoff = allocations[0].start()
            assert banner < handoff < output.index(READY), "startup did not reserve exactly the needed rows"
        assert b"a=d,d=A" not in output and b"i=4294967293" not in output, "mascot deletion/collision"
    finally:
        if proc.poll() is None:
            os.killpg(proc.pid, signal.SIGTERM)
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait(timeout=5)
        os.close(master)


def kitty_layout_inside():
    # Exercise the installed Kitty parser without opening or changing any of
    # the user's windows. It answers the local probes at the actual cursor.
    from kitty.fast_data_types import Screen, set_options
    from kitty.options.types import defaults
    set_options(defaults)

    class KittyTerminal:
        def __init__(self, rows, start_row):
            self.rows, self.columns, self.start_row = rows, 100, start_row
            self.reply = b""
            self.screen = Screen(self, rows, self.columns, 1000, 10, 20, 0, self)
            self.feed(b"local history\r\n" * start_row)

        def write(self, data):
            self.reply += bytes(data)

        def on_da1(self):
            self.write(b"\x1b[?62;c")

        def __getattr__(self, name):
            # Non-rendering window callbacks (title, focus, etc.) are irrelevant.
            return lambda *args: None

        def feed(self, data):
            data = memoryview(data)
            while data:
                destination = self.screen.test_create_write_buffer()
                count = self.screen.test_commit_write_buffer(data, destination)
                data = data[count:]
                self.screen.test_parse_written_data(None)
            reply, self.reply = self.reply, b""
            return reply

        def check_layout(self, show_mascot):
            lines = [str(self.screen.line(row)).rstrip() for row in range(self.rows)]
            ready = lines.index(READY.decode())
            if show_mascot:
                banner = lines.index("Goblin Skiff")
                assert ready == banner + 1, ("blank gap after mascot", banner, ready, lines)
                assert banner >= 8, ("mascot scrolled off a short session", lines)
            if not self.start_row:
                assert self.screen.historybuf.count == 0, ("fresh window scrolled unnecessarily", lines)

    for rows, start_row in ((48, 0), (48, 12), (24, 22), (80, 0)):
        session([], True, False, (True, True), terminal=KittyTerminal(rows, start_row))
    session(["--no-kitty", "--no-sixel"], False, False, (False, False), terminal=KittyTerminal(48, 0))
    session(["--no-mascot"], True, False, (True, True), show_mascot=False, terminal=KittyTerminal(48, 0))
    print("startup-screen: Kitty parser, fresh/mid-screen/bottom layouts passed")


def kitty_layout():
    kitty = shutil.which("kitty")
    if not kitty:
        print("startup-screen: Kitty unavailable; native layout subtest skipped")
        return
    env = os.environ.copy()
    env["GOBLIN_SKIFF_TEST_PYTHON"] = sys.executable
    # Packaged Kitty may run Python with -O; keep assertions in this test.
    code = (f"import sys; sys.argv = [{str(SCRIPT)!r}, '--kitty-layout-inside']; "
            f"exec(compile(open({str(SCRIPT)!r}, 'rb').read(), {str(SCRIPT)!r}, 'exec', optimize=0), "
            f"{{'__name__': '__main__', '__file__': {str(SCRIPT)!r}}})")
    subprocess.run([kitty, "+runpy", code], env=env, check=True, timeout=120)


def history_inside():
    print("LOCAL-HISTORY-BEFORE-MOSH", flush=True)
    result = subprocess.run(command(["--mascot=ascii", "--no-kitty", "--no-sixel"],
                                    ["printf", "REMOTE-HISTORY-AFTER-MOSH\\n"]))
    assert result.returncode == 0
    print("HISTORY-TEST-COMPLETE", flush=True)
    # Keep this test's tmux pane alive for capture-pane, without a timer race.
    sys.stdin.read(1)


def history():
    if not shutil.which("tmux"):
        print("startup-screen: tmux unavailable; history capture subtest skipped")
        return
    with tempfile.TemporaryDirectory(prefix="goblin-startup-") as directory:
        tmux = ["tmux", "-S", str(Path(directory) / "tmux.sock"), "-f", "/dev/null"]
        env = os.environ.copy()
        env.pop("TMUX", None)
        env.pop("MOSH_ESCAPE_KEY", None)
        env["TERM"] = "xterm-256color"
        try:
            subprocess.run([*tmux, "new-session", "-d", "-s", "history", "-x", "80", "-y", "24",
                            shlex.join([sys.executable, str(SCRIPT), "--history-inside"])],
                           env=env, check=True, timeout=10)
            deadline = time.monotonic() + 20
            while True:
                capture = subprocess.check_output([*tmux, "capture-pane", "-p", "-S", "-", "-t", "history"],
                                                  env=env, timeout=5)
                if b"HISTORY-TEST-COMPLETE" in capture:
                    break
                assert time.monotonic() < deadline, "history capture timeout: " + repr(capture[-300:])
                time.sleep(0.05)
            before = capture.index(b"LOCAL-HISTORY-BEFORE-MOSH")
            notice = capture.index(SOURCE_NOTICE.rstrip(b"\r\n"))
            panel_notice = capture.index(PANEL_NOTICE.rstrip(b"\r\n"))
            chicken = capture.index(b".+%%%%#=.")
            banner = capture.index(b"Goblin Skiff\n")
            after = capture.index(b"REMOTE-HISTORY-AFTER-MOSH")
            assert before < chicken < banner < after, "local output and chicken not retained in history"
            assert before < notice < panel_notice < chicken, "startup notices not retained ahead of the mascot"
            visible = subprocess.check_output([*tmux, "capture-pane", "-p", "-t", "history"], env=env, timeout=5)
            assert b".+%%%%#=." in visible and b"REMOTE-HISTORY-AFTER-MOSH" in visible, \
                "chicken must remain visible above a short remote session, not immediately scroll out"
        finally:
            subprocess.run([*tmux, "kill-server"], env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=5)


def main():
    if sys.argv[1:] in (["--remote"], ["--remote-graphics"]):
        remote(sys.argv[1] == "--remote-graphics")
        return
    if sys.argv[1:] == ["--history-inside"]:
        history_inside()
        return
    if sys.argv[1:] == ["--kitty-layout-inside"]:
        kitty_layout_inside()
        return
    if sys.argv[1:] == ["--kitty-layout"]:
        kitty_layout()
        return
    session([], True, False, (True, True))
    session(["--no-kitty"], False, True, (False, True))
    session(["--no-sixel"], True, False, (True, False))
    session(["--no-kitty", "--no-sixel"], False, False, (False, False))
    session(["--no-kitty", "--mascot=kitty"], False, False, (False, True))
    session(["--mascot=sixel", "--no-sixel"], True, False, (True, False))
    session([], True, False, (True, True), graphics=True)
    session(["--no-kitty", "--no-sixel"], False, False, (False, False), graphics=True)
    session(["--no-mascot"], True, False, (True, True), show_mascot=False)
    for prefix, notice in (("\x02", b"Control panel: Ctrl-B, release, then 0.\r\n"),
                           ("~", b'Control panel: Enter, then "~", then 0.\r\n'),
                           ("", b"Control panel shortcut disabled by MOSH_ESCAPE_KEY.\r\n"),
                           ("0", b"Control panel shortcut disabled by MOSH_ESCAPE_KEY.\r\n")):
        session(["--no-kitty", "--no-sixel"], False, False, (False, False), escape_key=prefix, notice=notice)
    for disabled in ("kitty", "sixel"):
        preview = subprocess.check_output([CLIENT, "--show-mascot=" + disabled, "--no-" + disabled])
        assert b"\x1b" not in preview and b".+%%%%#=." in preview, "preview ignored disable override"
    history()
    kitty_layout()
    print("startup-screen: inline history, graphics overrides, and clean exit passed")


if __name__ == "__main__":
    main()
