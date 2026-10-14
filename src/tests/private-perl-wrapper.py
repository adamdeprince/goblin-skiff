"""Exercise generated SSH helpers with a private Perl and no Perl on PATH."""
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading


PAYLOAD = b"login bytes\n"


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(5)
        data = b""
        while len(data) < len(PAYLOAD):
            part = self.request.recv(len(PAYLOAD) - len(data))
            if not part:
                return
            data += part
        self.request.sendall(b"SSH-direct\n" + data)


def test():
    source = Path(__file__).resolve().parent
    wrapper = Path(os.environ.get("GOBLIN_SKIFF_TEST_WRAPPER", "../../scripts/goblin-skiff")).resolve()
    spec = importlib.util.spec_from_file_location("socks_fixture", source / "socks5-integration.py")
    socks = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(socks)
    perl = shutil.which("perl")
    assert perl, "Perl is needed on the test host to model the bundled interpreter"

    # Spaces and apostrophes also exercise quoting of both interpreter and script.
    with tempfile.TemporaryDirectory(prefix="skiff's private perl ") as directory:
        root = Path(directory)
        (root / "bin").mkdir()
        (root / "private runtime").mkdir()
        private_perl = root / "private runtime/perl"
        private_perl.symlink_to(Path(perl).resolve())
        launcher = root / "skiff's launcher.pl"
        shutil.copy2(wrapper, launcher)
        launcher.chmod(0o700)
        for name in ("ssh", "client"):
            stub = root / "bin" / name
            stub.write_text("#!/bin/sh\nexec %s %s --fake-%s \"$@\"\n" % (
                shlex.quote(sys.executable), shlex.quote(str(source / "udp-jump-wrapper.py")), name))
            stub.chmod(0o700)
        logfile = root / "calls.jsonl"
        dictionary = root / "test dictionary.zst"
        dictionary.write_bytes(b"fixture dictionary")
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("GOBLIN_SKIFF_JUMP_") and not key.startswith("GOBLIN_SKIFF_TEST_")}
        env.update(PATH=str(root / "bin"), TERM="xterm-256color", GOBLIN_SKIFF_JUMP_TEST_LOG=str(logfile))
        assert shutil.which("perl", path=env["PATH"]) is None
        bare = subprocess.run([str(launcher), "--version"], env=env, capture_output=True, timeout=5)
        assert bare.returncode != 0 and b"perl" in bare.stderr, bare

        with socketserver.TCPServer(("127.0.0.1", 0), Echo) as direct, socks.proxy() as proxy:
            worker = threading.Thread(target=direct.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
            worker.start()
            checked = 0
            try:
                proxy_option = "--socks5-proxy=" + proxy.endpoint
                dict_option = "--state-zstd-dict=" + str(dictionary)
                for options in ([], [proxy_option], ["-J", "jump-a"], [dict_option], [proxy_option, dict_option]):
                    logfile.write_text("")
                    result = subprocess.run([
                        str(private_perl), str(launcher), "--no-mascot",
                        "--client=" + str(root / "bin/client"),
                        "--ssh=" + shlex.quote(str(root / "bin/ssh")),
                    ] + options + ["destination"], env=env, capture_output=True, timeout=10)
                    assert result.returncode == 0, (options, result.stdout, result.stderr)
                    calls = [json.loads(line) for line in logfile.read_text().splitlines()]
                    for call in calls:
                        if call["kind"] != "ssh":
                            continue
                        command = next((arg.split("=", 1)[1] for arg in call["args"]
                                        if arg.startswith("ProxyCommand=")), None)
                        if command is None:
                            continue
                        proxied = proxy_option in command
                        host = "only-in-tailnet.invalid" if proxied else "127.0.0.1"
                        port = 22 if proxied else direct.server_address[1]
                        command = command.replace("%h", host).replace("%p", str(port))
                        result = subprocess.run(["/bin/sh", "-c", command], input=PAYLOAD,
                                                env=env, capture_output=True, timeout=5)
                        assert result.returncode == 0, (options, command, result.stderr)
                        expected = (b"SSH-through-SOCKS\n" if proxied else b"SSH-direct\n") + PAYLOAD
                        assert result.stdout == expected, (options, result.stdout, result.stderr)
                        checked += 1
                assert checked == 7, checked
                assert proxy.requests == [(1, "only-in-tailnet.invalid", 22)] * 3, proxy.requests
            finally:
                direct.shutdown()
                worker.join(2)
    print("PASS: direct, SOCKS5, jump and dictionary SSH helpers reuse private Perl without Perl on PATH")


if __name__ == "__main__":
    test()
