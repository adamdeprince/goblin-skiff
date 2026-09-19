# Modified for Goblin Skiff on 2026-09-19.
"""Inline download protocol over real PTYs, encrypted UDP loss, and a stopped disk worker."""
import base64
import errno
import fcntl
import importlib.util
import os
from pathlib import Path
import pty
import random
import re
import select
import signal
import shutil
import struct
import subprocess
import sys
import tempfile
import termios
import time
import tty

SCRIPT = Path(__file__).resolve()
PREFIX = b"\x1b]777;goblin-download;v=1:"
ST = b"\x1b\\"


def blob():
    rng = random.Random(9126)
    return bytes(rng.randrange(256) for _ in range(48 * 1024))


def osc(meta, payload=None):
    return PREFIX + meta.encode() + (b";" + base64.b64encode(payload) if payload is not None else b"") + ST


def remote(enabled, allow):
    tty.setraw(0)
    incoming = bytearray()
    pending = []

    def events(timeout=0.1):
        ready, _, _ = select.select([0], [], [], timeout)
        if ready:
            data = os.read(0, 16384)
            if not data:
                raise RuntimeError("input closed")
            incoming.extend(data)
        while incoming:
            if incoming[0] == 27:
                end = incoming.find(ST)
                if end < 0:
                    break
                packet = bytes(incoming[:end + 2])
                del incoming[:end + 2]
                assert packet.startswith(PREFIX), packet
                pending.append(packet)
            else:
                pending.append(bytes(incoming[:1]))
                del incoming[:1]
        result, pending[:] = pending[:], []
        return result

    os.write(1, osc("op=query:id=123"))
    deadline = time.monotonic() + 10
    while True:
        packets = events()
        if packets:
            expected = b"status=supported:max_chunk=4096" if enabled else b"status=unsupported"
            assert len(packets) == 1 and expected in packets[0], packets
            break
        assert time.monotonic() < deadline, "no capability reply"
    if not enabled:
        os.write(1, b"\x1b[HUNSUPPORTED")
        while b"q" not in events():
            pass
        return
    data = blob()
    name = base64.b64encode(b"alpine.bin").decode()
    os.write(1, osc(f"op=begin:id=124:name={name}:size={len(data)}:reply=1") + b"\x1b[HBEGIN_READY")
    pulse = 0
    sent = False
    saved = False
    while True:
        for packet in events():
            if packet == b"s" and not sent:
                for at in range(0, len(data), 4096):
                    os.write(1, osc("op=data:id=124", data[at:at + 4096]))
                os.write(1, osc("op=end:id=124") + b"\x1b[3;1HTRANSFER_PENDING")
                sent = True
            elif packet == b"z":
                os.write(1, b"\x1b[5;1HKEYBOARD_OK")
            elif packet == b"q":
                assert saved, "quit before save"
                return
            elif packet.startswith(PREFIX):
                if b"status=queued" in packet:
                    os.write(1, b"\x1b[7;1HSTATUS_QUEUED")
                elif b"status=saved" in packet:
                    assert b"name=" + base64.b64encode(b"alpine.bin.1") in packet, packet
                    saved = True
                    os.write(1, b"\x1b[9;1HSTATUS_SAVED")
                elif not allow and b"status=error" in packet:
                    message = base64.b64decode(re.search(rb":message=([^\x1b]+)", packet)[1])
                    assert b"declined" in message.lower() or b"EPERM" in message, packet
                    saved = True
                    os.write(1, b"\x1b[9;1HSTATUS_DECLINED")
                else:
                    raise RuntimeError(repr(packet))
        pulse += 1
        os.write(1, b"\x1b[20;1H" + bytes([65 + pulse % 26]) * 24)


def session(enabled, allow=True, parent="local", actual_kitty=False):
    spec = importlib.util.spec_from_file_location("download_relay", SCRIPT.with_name("control-panel-integration.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    build = Path.cwd()
    server = str((build / "../frontend/goblin-skiff-server").resolve())
    client = str((build / "../frontend/goblin-skiff-client").resolve())
    with tempfile.TemporaryDirectory(prefix="goblin-download-live-") as root, \
            tempfile.TemporaryDirectory(prefix="goblin-download-parent-") as parent_root:
        destination = Path(root)
        (destination / "alpine.bin").write_bytes(b"existing file")
        env = os.environ.copy()
        env.update(TERM="xterm-256color", MOSH_CLIENT_CAPS="keepalive-v1,sixel-state-v1,goblin-download-v2",
                   MOSH_SERVER_NETWORK_TMOUT="45")
        bootstrap = subprocess.run([server, "new", "-i", "127.0.0.1", "-c", "256", "--", shutil.which("python3") or sys.executable,
                                    str(SCRIPT), "--remote", str(int(enabled)), str(int(allow))], stdin=subprocess.DEVNULL,
                                   capture_output=True, env=env, timeout=10)
        assert bootstrap.returncode == 0, bootstrap.stderr
        assert b"MOSH DOWNLOADS goblin-download-v2" in bootstrap.stdout
        match = re.search(rb"MOSH CONNECT (\d+) (\S+)", bootstrap.stdout)
        assert match, bootstrap.stdout
        server_pid = int(re.search(rb"detached, pid = (\d+)", bootstrap.stderr)[1])
        relay = module.Relay(int(match[1]))
        env.update(MOSH_KEY=match[2].decode(), GOBLIN_SKIFF_SIXEL_STATE="1", GOBLIN_SKIFF_DOWNLOADS=str(int(enabled)),
                   GOBLIN_SKIFF_DOWNLOAD_DIR=root, GOBLIN_SKIFF_COMPACT_KEEPALIVE="1", GOBLIN_SKIFF_MASCOT="none",
                   MOSH_PREDICTION_DISPLAY="never", MOSH_NO_TERM_INIT="1")
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 800, 480))
        process = subprocess.Popen([client, "--no-kitty", "--no-sixel", "127.0.0.1", str(relay.socket.getsockname()[1])],
                                   env=env, stdin=slave, stdout=slave, stderr=slave, start_new_session=True,
                                   preexec_fn=lambda: fcntl.ioctl(0, termios.TIOCSCTTY, 0))
        os.close(slave)
        relay.thread.start()
        output = bytearray()
        stopped = set()
        parent_scan = 0
        parent_request = None
        parent_decided = False
        parent_payload = bytearray()
        parent_file = None
        parent_finished = False
        parent_frames = []
        kitty_engine = None
        if actual_kitty:
            # Run this optional mode with `kitty +runpy`. Use its actual parser,
            # permission probe, writer and replies, without creating a GUI.
            # Only the user decision and destination root are test fixtures.
            import kitty.file_transmission as ft
            from types import SimpleNamespace
            ft.get_options = lambda: SimpleNamespace(file_transfer_confirmation_bypass="")
            (Path(parent_root) / "alpine.bin").write_bytes(b"existing file")

            class KittyEngine(ft.FileTransmission):
                def write_ftc_to_child(self, payload, **kwargs):
                    os.write(master, b"\x1b]" + payload.serialize(prefix_with_osc_code=True).encode() + ST)
                    return True

                def start_receive(self, identifier):
                    if self.active_receives[identifier].bypass_ok is not None:
                        super().start_receive(identifier)
                    else:
                        assert not parent_decided, "actual request bypassed permission wait"

            kitty_engine = KittyEngine(0)

        def kitty_status(identifier, status, extra=""):
            if kitty_engine is not None:
                return
            os.write(master, b"\x1b]5113;ac=status;id=" + identifier.encode() + b";st="
                     + base64.b64encode(status.encode()) + extra.encode() + ST)

        def service_parent():
            nonlocal parent_scan, parent_request, parent_file, parent_finished
            pattern = rb"\x1b](777;goblin-download;[^\x1b\x07]*|5113;[^\x1b\x07]*)(?:\x1b\\|\x07)"
            for match in re.finditer(pattern, bytes(output[parent_scan:])):
                packet = match[1].decode()
                parent_frames.append(packet)
                if packet.startswith("777;"):
                    meta, _, payload = packet[len("777;goblin-download;"):].partition(";")
                    fields = dict(item.split("=", 1) for item in meta.split(":"))
                    identifier = fields["id"]
                    if fields["op"] == "query":
                        response = "supported:max_chunk=4096:max_size=67108864:approval=1" if parent == "goblin" else "unsupported"
                        os.write(master, osc(f"op=status:id={identifier}:status={response}"))
                    elif fields["op"] == "begin":
                        assert parent == "goblin" and fields["confirm"] == "1", fields
                        assert base64.b64decode(fields["name"]) == b"alpine.bin"
                        parent_request = fields
                    elif fields["op"] == "data":
                        assert parent_decided and allow and parent == "goblin"
                        parent_payload.extend(base64.b64decode(payload))
                    elif fields["op"] == "end":
                        assert bytes(parent_payload) == blob()
                        os.write(master, osc(f"op=status:id={identifier}:status=saved:name="
                                             + base64.b64encode(b"alpine.bin.1").decode()))
                        parent_finished = True
                    else:
                        assert fields["op"] == "cancel", fields
                else:
                    fields = dict(item.split("=", 1) for item in packet[5:].split(";"))
                    identifier = fields["id"]
                    if fields["ac"] == "send" and "pw" in fields:
                        assert base64.b64decode(fields["pw"]) == b"sha256:0" and len(fields) == 3
                        if parent == "kitty":
                            kitty_status(identifier, "EPERM:Invalid probe authorization")
                    elif fields["ac"] == "send":
                        assert parent == "kitty" and len(fields) == 2
                        parent_request = fields
                    elif fields["ac"] == "file":
                        assert parent_decided and allow and parent == "kitty"
                        name = base64.b64decode(fields["n"])
                        assert fields["prm"] == "384" and fields["sz"] == str(len(blob()))
                        if fields["fid"] == "f0":
                            assert name == b"~/Downloads/alpine.bin"
                            kitty_status(identifier, "STARTED", ";fid=f0;sz=13")
                        else:
                            assert fields["fid"] == "f1" and name == b"~/Downloads/alpine.bin.1"
                            parent_file = fields
                            kitty_status(identifier, "STARTED", ";fid=f1;sz=-1")
                    elif fields["ac"] in ("data", "end_data"):
                        assert parent_file and fields["fid"] == "f1"
                        chunk = base64.b64decode(fields.get("d", ""))
                        assert len(chunk) <= 4096
                        parent_payload.extend(chunk)
                        if fields["ac"] == "end_data":
                            assert bytes(parent_payload) == blob()
                        kitty_status(identifier, "OK" if fields["ac"] == "end_data" else "PROGRESS",
                                     f";fid=f1;sz={len(parent_payload)}")
                    elif fields["ac"] == "finish":
                        assert bytes(parent_payload) == blob()
                        parent_finished = True
                    else:
                        assert fields["ac"] == "cancel", fields
                    if kitty_engine is not None:
                        cmd = ft.FileTransmissionCommand.deserialize(memoryview(packet[5:].encode()))
                        if cmd.name:
                            assert cmd.name in ("~/Downloads/alpine.bin", "~/Downloads/alpine.bin.1")
                            # Do not alter HOME or touch the real Downloads.
                            cmd.name = str(Path(parent_root) / Path(cmd.name).name)
                        kitty_engine.handle_serialized_command(memoryview(cmd.serialize().encode()))
                last = match.end()
            if 'last' in locals():
                parent_scan += last

        def collect(duration=0.1):
            deadline = time.monotonic() + duration
            while time.monotonic() < deadline:
                readable, _, _ = select.select([master], [], [], max(0, deadline - time.monotonic()))
                if not readable:
                    return
                try:
                    data = os.read(master, 65536)
                except OSError as error:
                    if error.errno != errno.EIO:
                        raise
                    return
                if not data:
                    return
                output.extend(data)
                service_parent()

        def until(marker, start=0, timeout=45):
            deadline = time.monotonic() + timeout
            while marker not in output[start:]:
                assert time.monotonic() < deadline, ("missing", marker, "first", bytes(output[start:start + 2500]),
                                                    "last", bytes(output[-1000:]))
                collect()

        try:
            if enabled:
                until(b"BEGIN_READY")
                if parent != "local":
                    deadline = time.monotonic() + 15
                    while parent_request is None:
                        assert time.monotonic() < deadline, "parent did not receive transfer offer"
                        collect()
                    os.write(master, b"s")
                    until(b"STATUS_QUEUED")
                    collect(0.3)
                    assert not parent_payload and not module.children(process.pid), "intermediate forwarded payload or touched disk before parent approval"
                    parent_decided = True
                    if parent == "goblin":
                        status = "approved" if allow else "error:message=" + base64.b64encode(b"Parent user declined").decode()
                        os.write(master, osc(f"op=status:id={parent_request['id']}:status={status}"))
                    elif kitty_engine is not None:
                        kitty_engine.handle_send_confirmation(allow, parent_request["id"])
                    else:
                        kitty_status(parent_request["id"], "OK" if allow else "EPERM:User declined")
                    until(b"STATUS_SAVED" if allow else b"STATUS_DECLINED", timeout=60)
                    collect(0.3)
                    assert parent_finished == allow
                    assert bytes(parent_payload) == (blob() if allow else b"")
                    assert not module.children(process.pid), "forwarding started a local filesystem worker"
                    assert set(p.name for p in destination.iterdir()) == {"alpine.bin"}, "forwarding saved an intermediate copy"
                    assert b"Remote download waiting" not in output and b"May we save this file to Downloads?" not in output
                    if kitty_engine is not None:
                        assert not kitty_engine.active_receives
                        saved = Path(parent_root) / "alpine.bin.1"
                        assert saved.exists() == allow
                        if allow:
                            assert saved.read_bytes() == blob() and saved.stat().st_mode & 0o777 == 0o600
                        assert (Path(parent_root) / "alpine.bin").read_bytes() == b"existing file"
                    os.write(master, b"q")
                    until(b"[goblin-skiff is exiting.]")
                    process.wait(timeout=5)
                    assert process.returncode == 0
                    print(f"Live parent={parent}, actual_engine={actual_kitty}, allow={allow}: exact bytes/status across encrypted loss relay; no intermediate popup, worker or file")
                    return
                until(b"May we save this file to Downloads?")
                collect(0.5)
                assert not module.children(process.pid), "download started worker before approval"
                assert set(p.name for p in destination.iterdir()) == {"alpine.bin"}, "unapproved download touched disk"
                os.write(master, b"\x1e0")
                until(b"Remote download waiting")
                os.write(master, b"s")
                until(b"STATUS_QUEUED")
                offset = len(output)
                os.write(master, b"\x1e0")
                until(b"May we save this file to Downloads?", offset)
                collect(0.2)
                if allow:
                    os.write(master, b"y")
                    # The display diff skips the unchanged space before "file".
                    until(b"Enter: save this", offset)
                    assert not module.children(process.pid), "Save selection without confirmation started a worker"
                os.write(master, b"\r")  # Confirm Save, or decline by default.
                if not allow:
                    until(b"STATUS_DECLINED")
                    assert not module.children(process.pid), "declined download created a worker"
                    assert set(p.name for p in destination.iterdir()) == {"alpine.bin"}, "declined download touched disk"
                    os.write(master, b"q")
                    until(b"[goblin-skiff is exiting.]")
                    process.wait(timeout=5)
                    assert process.returncode == 0
                    print("Live download decline: no worker, no disk writes, remote error delivered")
                    return
                deadline = time.monotonic() + 10
                helpers = set()
                while not helpers:
                    assert time.monotonic() < deadline, "download did not start worker"
                    collect()
                    helpers = module.children(process.pid)
                assert len(helpers) == 1, helpers
                for pid in helpers:
                    os.kill(pid, signal.SIGSTOP)
                    stopped.add(pid)
                offset = len(output)
                os.write(master, b"z")
                until(b"KEYBOARD_OK", offset, timeout=5)
                collect(1.5)
                pulses = re.findall(rb"([A-Z])\1{15}", output[offset:])
                assert len(set(pulses)) >= 3, "stalled disk froze screen updates"
                assert not (destination / "alpine.bin.1").exists(), "final name exposed before completion"
                assert helpers == module.children(process.pid), "stalled disk spawned extra workers"
                for pid in list(stopped):
                    os.kill(pid, signal.SIGCONT)
                    stopped.remove(pid)
                until(b"STATUS_SAVED", timeout=60)
                assert (destination / "alpine.bin").read_bytes() == b"existing file"
                assert (destination / "alpine.bin.1").read_bytes() == blob()
                assert set(p.name for p in destination.iterdir()) == {"alpine.bin", "alpine.bin.1"}
                assert (destination / "alpine.bin.1").stat().st_mode & 0o777 == 0o600
            else:
                until(b"UNSUPPORTED")
                assert not module.children(process.pid), "disabled feature started disk worker"
            assert all("op=query" in frame or frame.startswith("5113;") for frame in parent_frames), "file payload leaked into local terminal fallback"
            os.write(master, b"q")
            until(b"[goblin-skiff is exiting.]")
            process.wait(timeout=5)
            assert process.returncode == 0
            print(f"Live download enabled={enabled}: encrypted loss relay, clipboard/graphics independence, file safety passed ({relay.dropped} dropped)")
        finally:
            for pid in stopped:
                os.kill(pid, signal.SIGCONT)
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
        remote(bool(int(sys.argv[2])), bool(int(sys.argv[3])))
    elif sys.argv[1:2] == ["--kitty-engine"]:
        session(True, parent="kitty", actual_kitty=True)
        session(True, False, parent="kitty", actual_kitty=True)
    else:
        session(True)
        session(True, False)
        session(False)
        session(True, parent="goblin")
        session(True, False, parent="goblin")
        session(True, parent="kitty")
        session(True, False, parent="kitty")
