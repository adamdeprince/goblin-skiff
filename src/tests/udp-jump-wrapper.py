"""Wrapper routing contract with a fake SSH transport; never contacts a host."""
import base64
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def log(kind, **values):
    with open(os.environ["GOBLIN_SKIFF_JUMP_TEST_LOG"], "a") as output:
        output.write(json.dumps(dict(kind=kind, **values)) + "\n")


def key(host):
    return base64.b64encode(hashlib.sha256(host.encode()).digest()[:16]).decode().rstrip("=")


def fake_ssh(args):
    if "-G" in args:
        log("config", args=args)
        jump = os.environ.get("GOBLIN_SKIFF_JUMP_CONFIG", "none") if args[-1] == "destination" else "none"
        if args[-1] == "jump-b" and os.environ.get("GOBLIN_SKIFF_JUMP_NESTED") == "1":
            jump = "jump-a"
        for index, arg in enumerate(args):
            if arg == "-J":
                jump = args[index + 1]
            elif arg.startswith("-J"):
                jump = arg[2:]
            elif arg.lower().startswith("-oproxyjump="):
                jump = arg.split("=", 1)[1]
        print("hostname target.invalid\nproxyjump " + jump)
        return 0
    command = args[-1]
    host = args[-3]
    words = shlex.split(command)
    if words[-3:-1] == ["sh", "-c"]:
        words = shlex.split(words[-1])  # uploaded-dictionary cleanup wrapper
    log("ssh", args=args, host=host, command=command)
    failure = os.environ.get("GOBLIN_SKIFF_JUMP_FAILURE", "")
    proxy_command = next((arg for arg in args if arg.startswith("ProxyCommand=")), "")
    socks_proxy = "--socks5-proxy=" in proxy_command
    if "MOSH DICT" in command:
        sys.stdin.buffer.read()
        print("MOSH DICT /tmp/goblin-test-dictionary.zst")
        return 0
    if "relay" in words:
        if failure == "old-jump":
            print("unsupported relay command", file=sys.stderr)
            return 1
        if proxy_command and (not socks_proxy or "'--proxy-report'" in proxy_command):
            print("MOSH IP first.tailnet.invalid" if socks_proxy else "MOSH IP 203.0.113.7")
        private = "10.0.0.2" if host == "jump-b" else "10.0.0.1"
        port = 62002 if host == "jump-b" else 62001
        mode = "aes128-gcm-v1" if "--fips-crypto" in words else "ocb-aes128"
        if failure == "wrong-suite":
            mode = "wrong-suite"
        print("MOSH RELAY 1 %s %s %d %s" % (mode, private, port, key(host)))
    else:
        hops = next((int(word.split("=", 1)[1]) for word in words if word.startswith("MOSH_RELAY_HOPS=")), 0)
        if hops:
            print("MOSH SSH_CONNECTION 10.20.30.1 40000 10.20.30.40 22")
            if failure != "old-destination":
                print("MOSH RELAY-MTU 1 %d" % hops)
        else:
            print("MOSH IP target.invalid" if socks_proxy else "MOSH IP 192.0.2.3")
        if "--fips-crypto" in words:
            print("MOSH CRYPTO aes128-gcm-v1")
        if failure != "old-image-server":
            caps = next((word for word in words if word.startswith("MOSH_CLIENT_CAPS=")), "")
            if "palette-djvu-v1" in caps:
                print("MOSH IMAGE palette-djvu-v1")
            elif any(word.startswith("--lossy=") for word in words):
                print("MOSH IMAGE webp")
        if failure != "unversioned-server":
            version = os.environ.get("GOBLIN_SKIFF_TEST_SERVER_VERSION", "1 1.4.0-goblin20260915.1 server-build")
            print("MOSH VERSION " + version)
            if failure == "duplicate-version":
                print("MOSH VERSION " + version)
        print("MOSH CAPS keepalive-v1\nMOSH CONNECT 61000 " + key("end-to-end"))
    return 0


def fake_client(args):
    if "--connection-version" in args:
        if os.environ.get("GOBLIN_SKIFF_JUMP_FAILURE") == "unversioned-client":
            return 1
        print("MOSH VERSION " + os.environ.get("GOBLIN_SKIFF_TEST_CLIENT_VERSION", "1 1.4.0-goblin20260915.1 client-build"))
        return 0
    if "--image-codecs" in args:
        if os.environ.get("GOBLIN_SKIFF_JUMP_FAILURE") == "old-image-client":
            return 1
        print("webp palette-djvu-v1")
        return 0
    if "-c" in args:
        log("preflight", args=args)
        if os.environ.get("GOBLIN_SKIFF_JUMP_FAILURE") == "old-client" and ("--udp-relay" in args or any(arg.startswith("--socks5-proxy=") for arg in args)):
            return 1
        print("256")
        return 0
    log("client", args=args, relay_keys=os.environ.get("GOBLIN_SKIFF_RELAY_KEYS"), key=os.environ.get("MOSH_KEY"))
    return 0


def test():
    wrapper = (Path.cwd() / "../../scripts/goblin-skiff").resolve()
    source = Path(__file__).resolve()
    with tempfile.TemporaryDirectory(prefix="goblin-jump-wrapper-", dir="/tmp") as root:
        root = Path(root)
        for name in ("ssh", "client"):
            path = root / name
            path.write_text("#!/bin/sh\nexec %s %s --fake-%s \"$@\"\n" %
                            (shlex.quote(sys.executable), shlex.quote(str(source)), name))
            path.chmod(0o700)
        logfile = root / "calls.jsonl"
        env = os.environ.copy()
        env.update(TERM="xterm-256color", GOBLIN_SKIFF_JUMP_TEST_LOG=str(logfile))
        for name in ("GOBLIN_SKIFF_RELAY_KEYS", "MOSH_RELAY_HOPS", "GOBLIN_SKIFF_JUMP_CONFIG", "GOBLIN_SKIFF_JUMP_FAILURE", "GOBLIN_SKIFF_JUMP_NESTED",
                     "GOBLIN_SKIFF_TEST_CLIENT_VERSION", "GOBLIN_SKIFF_TEST_SERVER_VERSION"):
            env.pop(name, None)

        def run(extra=(), config="none", failure="", ssh_options=(), success=True, diagnostic=None):
            logfile.write_text("")
            case_env = dict(env, GOBLIN_SKIFF_JUMP_CONFIG=config, GOBLIN_SKIFF_JUMP_FAILURE=failure)
            args = [str(wrapper), "--no-mascot", "--client=" + str(root / "client"),
                    "--ssh=" + shlex.join([str(root / "ssh")] + list(ssh_options))]
            proc = subprocess.run(args + list(extra) + ["destination"], env=case_env, capture_output=True, timeout=10)
            calls = [json.loads(line) for line in logfile.read_text().splitlines()]
            assert (proc.returncode == 0) == success, (extra, failure, proc.returncode, proc.stderr, proc.stdout)
            if not success:
                assert not any(call["kind"] == "client" for call in calls), "failure started client anyway"
            if diagnostic:
                assert diagnostic.encode() in proc.stderr, proc.stderr
            return calls

        def check_chain(calls):
            ssh = [call for call in calls if call["kind"] == "ssh"]
            assert [call["host"] for call in ssh] == ["destination", "jump-b", "jump-a"]
            assert "ProxyCommand=" not in " ".join(ssh[0]["args"]), "default proxy overrode ProxyJump"
            assert "MOSH_RELAY_HOPS=2" in shlex.split(ssh[0]["command"])
            assert shlex.split(ssh[1]["command"])[-2:] == ["10.20.30.40", "61000"]
            assert shlex.split(ssh[2]["command"])[-2:] == ["10.0.0.2", "62002"]
            assert ssh[1]["args"][ssh[1]["args"].index("-J") + 1] == "jump-a"
            assert any("ProxyCommand=" in arg for arg in ssh[2]["args"])
            client = next(call for call in calls if call["kind"] == "client")
            assert client["args"][-2:] == ["203.0.113.7", "62001"], "client attempted private target address"
            assert client["relay_keys"] == key("jump-a") + "," + key("jump-b")
            assert client["key"] == key("end-to-end")
            assert all(key("end-to-end") not in call["command"] for call in ssh), "terminal key exposed to relay"
            assert "--udp-relay" in client["args"]
            assert "--udp-relay" in next(call for call in calls if call["kind"] == "preflight")["args"]
            return ssh

        check_chain(run(["-J", "jump-a,jump-b"]))
        check_chain(run(config="jump-a,jump-b"))
        env["GOBLIN_SKIFF_JUMP_NESTED"] = "1"
        check_chain(run(config="jump-b"))
        del env["GOBLIN_SKIFF_JUMP_NESTED"]
        check_chain(run(ssh_options=["-J", "jump-a,jump-b"]))
        check_chain(run(ssh_options=["-oProxyJump=jump-a,jump-b"]))
        ssh = check_chain(run(["--fips-crypto", "-J", "jump-a,jump-b", "--jump-port=60100:60200"],
                             ssh_options=["-F", "config with spaces", "-p", "2222", "-l", "target-user"]))
        for call in ssh:
            assert "--fips-crypto" in shlex.split(call["command"])
        for call in ssh[1:]:
            assert call["args"][call["args"].index("-F") + 1] == "config with spaces"
            assert "2222" not in call["args"] and "target-user" not in call["args"]
            assert "--port=60100:60200" in shlex.split(call["command"])
        calls = run(["-J", "user@[2001:db8::5]:2202"])
        hop = [call for call in calls if call["kind"] == "ssh"][1]
        assert hop["host"] == "user@2001:db8::5" and hop["args"][hop["args"].index("-p") + 1] == "2202"
        for failure in ("old-client", "old-destination", "old-jump", "wrong-suite"):
            calls = run(["-J", "jump-a"], failure=failure, success=False)
            if failure == "old-client":
                assert not any(call["kind"] == "ssh" for call in calls), "old client started a remote server"
        for value in ("a,b,c,d,e", "a,", "a:65536", "a:0", "bad/host", "-bad"):
            run(["--jump=" + value], success=False)
        run(["--local", "-J", "jump-a"], success=False)
        run(["-J", "jump-a", "--jump-port=1:0"], success=False)
        run(["-J", "jump-a", "--jump-idle-timeout=15"], success=False)
        direct = run()
        client = next(call for call in direct if call["kind"] == "client")
        assert client["relay_keys"] is None and client["args"][-2:] == ["192.0.2.3", "61000"]
        for route in ("none", "jump-a", "jump-a,jump-b", "jump-a,jump-b,jump-c,jump-d"):
            proxy_option = "--socks5-proxy=127.0.0.1:1055"
            calls = run([proxy_option], config=route)
            client = next(call for call in calls if call["kind"] == "client")
            assert proxy_option in client["args"]
            assert proxy_option in next(call for call in calls if call["kind"] == "preflight")["args"]
            assert client["args"][-2] == ("target.invalid" if route == "none" else "first.tailnet.invalid")
            ssh_calls = [call for call in calls if call["kind"] == "ssh"]
            hops = 0 if route == "none" else len(route.split(","))
            assert len(ssh_calls) == hops + 1
            for position, call in enumerate(ssh_calls):
                command = next(arg.split("=", 1)[1] for arg in call["args"] if arg.startswith("ProxyCommand="))
                assert proxy_option in command
                # Inspect each nested OpenSSH ProxyCommand after its parent's
                # percent expansion. Inner hosts must not inherit outer %h/%p.
                depth = hops if position == 0 else hops - position
                for level in range(depth):
                    # OpenSSH expands %% to %, plus its own target tokens.
                    expanded = command.replace("%%", "\0").replace("%h", "current-target").replace("%p", "22").replace("\0", "%")
                    argv = shlex.split(expanded)
                    assert argv[argv.index("-W") + 1] == "[current-target]:22"
                    assert argv[-1] == route.split(",")[depth - level - 1]
                    command = next(arg.split("=", 1)[1] for arg in argv if arg.startswith("ProxyCommand="))
                assert "'%h'" in command and "'%p'" in command, command
                assert ("--proxy-report" in command) == (depth == 0)
        old = run(["--socks5-proxy=127.0.0.1:1055"], failure="old-client", success=False)
        assert not any(call["kind"] == "ssh" for call in old)
        for value in ("localhost", "user@host:1055", "socks5://host:1055", "host:0", "host:65536", "::1:1055"):
            run(["--socks5-proxy=" + value], success=False)
        run(["--socks5-proxy=127.0.0.1:1055", "--local"], success=False)
        dictionary = root / "dictionary with spaces.zst"
        dictionary.write_bytes(b"fixture dictionary")
        for route in ("none", "jump-a,jump-b"):
            calls = run(["--socks5-proxy=127.0.0.1:1055", "--state-zstd-dict=" + str(dictionary)], config=route,
                        ssh_options=["-S", "existing-control-master"])
            ssh = [call for call in calls if call["kind"] == "ssh"]
            assert "MOSH DICT" in ssh[0]["command"]
            for call in ssh[:2]:
                assert "--socks5-proxy=" in next(arg for arg in call["args"] if arg.startswith("ProxyCommand="))
                last_control = max(i for i, arg in enumerate(call["args"]) if arg == "-S")
                assert call["args"][last_control + 1] == "none", "existing SSH master could bypass the proxy"
        for options in (["--lossy=0"], ["--lossy", "100"], ["--djvu-lossy"], ["--lossy=75", "--djvu-lossy"]):
            calls = run(options)
            words = shlex.split(next(call["command"] for call in calls if call["kind"] == "ssh"))
            caps = next(word for word in words if word.startswith("MOSH_CLIENT_CAPS="))
            assert "palette-djvu-v1" in caps
            assert ("--djvu-lossy" in words) == ("--djvu-lossy" in options)
            expected_quality = "100" if options == ["--lossy", "100"] else next(
                (arg.split("=", 1)[1] for arg in options if arg.startswith("--lossy=")), None)
            assert next((arg.split("=", 1)[1] for arg in words if arg.startswith("--lossy=")), None) == expected_quality
        for quality in ("-1", "101", "", "no", "1.5", "nan", " 50", "9999999999999999999999"):
            calls = run(["--lossy=" + quality], success=False)
            assert not any(call["kind"] == "ssh" for call in calls)
        run(["--lossy=75"], failure="old-image-server", success=False)
        run(["--djvu-lossy"], failure="old-image-server", success=False)
        for options in ([], ["--lossy=75"]):
            calls = run(options, failure="old-image-client")
            assert "palette-djvu-v1" not in next(call["command"] for call in calls if call["kind"] == "ssh")
        calls = run(["--djvu-lossy"], failure="old-image-client", success=False)
        assert not any(call["kind"] == "ssh" for call in calls)
        # Releases differ freely; protocol and the actual selected binary decide compatibility.
        env["GOBLIN_SKIFF_TEST_SERVER_VERSION"] = "1 1.4.0-goblin20270101.1 future-build"
        run(diagnostic="server 1.4.0-goblin20270101.1; protocol 1")
        run(failure="unversioned-server", diagnostic="server unversioned")
        run(failure="unversioned-client", diagnostic="client unversioned")
        env["GOBLIN_SKIFF_TEST_SERVER_VERSION"] = "2 2.0.0 future-build"
        calls = run(["-J", "jump-a"], success=False, diagnostic="Incompatible Goblin Skiff protocols")
        assert len([call for call in calls if call["kind"] == "ssh"]) == 1, "incompatible session started relays"
        env["GOBLIN_SKIFF_TEST_CLIENT_VERSION"] = "2 2.0.1 newer-client"
        run(diagnostic="protocol 2")  # wrapper must not compare its own protocol
        del env["GOBLIN_SKIFF_TEST_CLIENT_VERSION"]
        for version in ("0 release build", "4294967296 release build", "1 release", "1 release bad\x1b[2J",
                        "1 " + "x" * 129 + " build"):
            env["GOBLIN_SKIFF_TEST_SERVER_VERSION"] = version
            run(success=False, diagnostic="Invalid MOSH VERSION")
        del env["GOBLIN_SKIFF_TEST_SERVER_VERSION"]
        run(failure="duplicate-version", success=False, diagnostic="Duplicate MOSH VERSION")
        env["GOBLIN_SKIFF_TEST_CLIENT_VERSION"] = "malformed"
        calls = run(success=False, diagnostic="Invalid MOSH VERSION")
        assert not any(call["kind"] == "ssh" for call in calls), "invalid client identity started SSH"
        print("PASS: connection versions, mixed releases, unversioned peers and protocol mismatch before relay setup")
        print("PASS: image quality flags, independent DjVu lossiness, validation and codec negotiation")
        print("PASS: explicit/configured/--ssh jumps, ordered chain setup, NAT address, IPv6, SSH ports/config, FIPS, preflight and fail-closed negotiation")
        print("PASS: SOCKS5 direct/1-4 hop setup, proxy DNS, nested percent expansion and old-client preflight")


if __name__ == "__main__":
    if sys.argv[1:2] == ["--fake-ssh"]:
        raise SystemExit(fake_ssh(sys.argv[2:]))
    if sys.argv[1:2] == ["--fake-client"]:
        raise SystemExit(fake_client(sys.argv[2:]))
    test()
