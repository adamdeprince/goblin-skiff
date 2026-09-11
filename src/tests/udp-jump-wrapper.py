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
    with open(os.environ["GOBLIN_JUMP_TEST_LOG"], "a") as output:
        output.write(json.dumps(dict(kind=kind, **values)) + "\n")


def key(host):
    return base64.b64encode(hashlib.sha256(host.encode()).digest()[:16]).decode().rstrip("=")


def fake_ssh(args):
    if "-G" in args:
        log("config", args=args)
        jump = os.environ.get("GOBLIN_JUMP_CONFIG", "none") if args[-1] == "destination" else "none"
        if args[-1] == "jump-b" and os.environ.get("GOBLIN_JUMP_NESTED") == "1":
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
    log("ssh", args=args, host=host, command=command)
    failure = os.environ.get("GOBLIN_JUMP_FAILURE", "")
    if "relay" in words:
        if failure == "old-jump":
            print("unsupported relay command", file=sys.stderr)
            return 1
        if any("ProxyCommand=" in arg for arg in args):
            print("MOSH IP 203.0.113.7")
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
            print("MOSH IP 192.0.2.3")
        if "--fips-crypto" in words:
            print("MOSH CRYPTO aes128-gcm-v1")
        print("MOSH CAPS keepalive-v1\nMOSH CONNECT 61000 " + key("end-to-end"))
    return 0


def fake_client(args):
    if "-c" in args:
        log("preflight", args=args)
        if os.environ.get("GOBLIN_JUMP_FAILURE") == "old-client" and "--udp-relay" in args:
            return 1
        print("256")
        return 0
    log("client", args=args, relay_keys=os.environ.get("MOSH_RELAY_KEYS"), key=os.environ.get("MOSH_KEY"))
    return 0


def test():
    wrapper = (Path.cwd() / "../../scripts/goblin-mosh").resolve()
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
        env.update(TERM="xterm-256color", GOBLIN_JUMP_TEST_LOG=str(logfile))
        for name in ("MOSH_RELAY_KEYS", "MOSH_RELAY_HOPS", "GOBLIN_JUMP_CONFIG", "GOBLIN_JUMP_FAILURE", "GOBLIN_JUMP_NESTED"):
            env.pop(name, None)

        def run(extra=(), config="none", failure="", ssh_options=(), success=True):
            logfile.write_text("")
            case_env = dict(env, GOBLIN_JUMP_CONFIG=config, GOBLIN_JUMP_FAILURE=failure)
            args = [str(wrapper), "--no-mascot", "--client=" + str(root / "client"),
                    "--ssh=" + shlex.join([str(root / "ssh")] + list(ssh_options))]
            proc = subprocess.run(args + list(extra) + ["destination"], env=case_env, capture_output=True, timeout=10)
            assert (proc.returncode == 0) == success, (extra, failure, proc.returncode, proc.stderr)
            calls = [json.loads(line) for line in logfile.read_text().splitlines()]
            if not success:
                assert not any(call["kind"] == "client" for call in calls), "failure started client anyway"
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
        env["GOBLIN_JUMP_NESTED"] = "1"
        check_chain(run(config="jump-b"))
        del env["GOBLIN_JUMP_NESTED"]
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
        print("PASS: explicit/configured/--ssh jumps, ordered chain setup, NAT address, IPv6, SSH ports/config, FIPS, preflight and fail-closed negotiation")


if __name__ == "__main__":
    if sys.argv[1:2] == ["--fake-ssh"]:
        raise SystemExit(fake_ssh(sys.argv[2:]))
    if sys.argv[1:2] == ["--fake-client"]:
        raise SystemExit(fake_client(sys.argv[2:]))
    test()
