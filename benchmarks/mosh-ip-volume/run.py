#!/usr/bin/env python3

"""Compare Mosh variants by IP packet volume over deterministic terminal workloads."""

from __future__ import annotations

import argparse
import base64
import csv
import datetime as dt
import fcntl
import hashlib
import ipaddress
import json
import os
import platform
import pty
import random
import re
import select
import shlex
import shutil
import signal
import socket
import statistics
import struct
import subprocess
import sys
import termios
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pcap_ip


MARKER = "MOSH_IP_BENCH"
DEFAULT_WORKLOADS = (
    "idle",
    "dashboard",
    "scroll-repetitive",
    "scroll-entropy",
    "full-redraw",
)
ALL_WORKLOADS = DEFAULT_WORKLOADS + ("kitty-place",)


@dataclass(frozen=True)
class Variant:
    name: str
    command: tuple[str, ...]


@dataclass(frozen=True)
class Job:
    variant: Variant
    workload: str
    repetition: int
    seed: int
    port: int


@dataclass
class ClientResult:
    exit_code: int
    ready_ns: int
    start_ns: int
    done_ns: int
    exit_ns: int
    updates: int
    digest: str


class VisibleAscii:
    """Collect printable bytes while discarding terminal control sequences."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.state = "text"
        self.osc = bytearray()

    def finish_osc(self) -> None:
        self.data.extend(self.osc)
        self.osc.clear()
        self.state = "text"

    def feed(self, chunk: bytes) -> None:
        for byte in chunk:
            if self.state == "text":
                if byte == 0x1B:
                    self.state = "escape"
                elif byte == 0x9B:
                    self.state = "csi"
                elif byte == 0x9D:
                    self.osc.clear()
                    self.state = "osc"
                elif byte in (0x90, 0x9E, 0x9F):
                    self.state = "string"
                elif 0x20 <= byte <= 0x7E:
                    self.data.append(byte)
            elif self.state == "escape":
                if byte == ord("["):
                    self.state = "csi"
                elif byte == ord("]"):
                    self.osc.clear()
                    self.state = "osc"
                elif byte in (ord("P"), ord("^"), ord("_"), ord("X")):
                    self.state = "string"
                elif 0x20 <= byte <= 0x2F:
                    self.state = "escape-intermediate"
                else:
                    self.state = "text"
            elif self.state == "escape-intermediate":
                if 0x30 <= byte <= 0x7E:
                    self.state = "text"
            elif self.state == "csi":
                if 0x40 <= byte <= 0x7E:
                    self.state = "text"
            elif self.state == "osc":
                if byte in (0x07, 0x9C):
                    self.finish_osc()
                elif byte == 0x1B:
                    self.state = "osc-escape"
                elif len(self.osc) < 4096:
                    self.osc.append(byte)
            elif self.state == "osc-escape":
                if byte == ord("\\"):
                    self.finish_osc()
                elif byte != 0x1B:
                    self.state = "osc"
            elif self.state == "string":
                if byte in (0x07, 0x9C):
                    self.state = "text"
                elif byte == 0x1B:
                    self.state = "string-escape"
            elif self.state == "string-escape":
                if byte == ord("\\"):
                    self.state = "text"
                elif byte != 0x1B:
                    self.state = "string"

        if len(self.data) > 2 * 1024 * 1024:
            del self.data[: len(self.data) - 1024 * 1024]


class Capture:
    def __init__(
        self,
        command: list[str],
        interface: str,
        server_ip: str,
        port: int,
        pcap_path: Path,
        stderr_path: Path,
    ) -> None:
        self.stderr_stream = stderr_path.open("wb")
        self.pcap_stream = pcap_path.open("wb")
        capture_filter = f"udp and host {server_ip} and port {port}"
        argv = command + [
            "-i",
            interface,
            "--immediate-mode",
            "-n",
            "-U",
            "-s",
            "0",
            "-w",
            "-",
            capture_filter,
        ]
        try:
            self.process = subprocess.Popen(
                argv,
                stdin=subprocess.DEVNULL,
                stdout=self.pcap_stream,
                stderr=self.stderr_stream,
                start_new_session=True,
            )
        except BaseException:
            self.stderr_stream.close()
            self.pcap_stream.close()
            raise
        deadline = time.monotonic() + 0.5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.stderr_stream.flush()
                message = stderr_path.read_text(errors="replace")
                self.stderr_stream.close()
                self.pcap_stream.close()
                raise RuntimeError(f"packet capture exited during startup:\n{message}")
            time.sleep(0.05)

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(self.process.pid, signal.SIGTERM)
                try:
                    self.process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    os.killpg(self.process.pid, signal.SIGKILL)
                    self.process.wait(timeout=5)
        self.stderr_stream.close()
        self.pcap_stream.close()
        if self.process.returncode not in (
            0,
            -signal.SIGINT,
            -signal.SIGTERM,
            130,
            143,
        ):
            raise RuntimeError(
                f"packet capture exited with status {self.process.returncode}"
            )


def parse_variant(value: str) -> Variant:
    if "=" not in value:
        raise argparse.ArgumentTypeError("variant must be NAME=COMMAND")
    name, command_text = value.split("=", 1)
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
        raise argparse.ArgumentTypeError(f"invalid variant name: {name}")
    command = tuple(shlex.split(command_text))
    if not command:
        raise argparse.ArgumentTypeError(f"empty command for variant {name}")
    return Variant(name, command)


def parse_args() -> argparse.Namespace:
    timestamp = dt.datetime.now().strftime("%Y%m%dT%H%M%S")
    default_results = Path(__file__).resolve().parent / "results" / timestamp
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="SSH/Mosh destination, such as adam@naamah")
    parser.add_argument(
        "--variant",
        action="append",
        type=parse_variant,
        help="NAME=COMMAND; repeat for each implementation",
    )
    parser.add_argument("--workload", action="append", choices=ALL_WORKLOADS)
    parser.add_argument("--duration", type=float, default=10.0)
    parser.add_argument("--hz", type=float, default=10.0)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--order-seed", type=int, default=20260829)
    parser.add_argument("--width", type=int, default=80)
    parser.add_argument("--height", type=int, default=24)
    parser.add_argument("--port-base", type=int, default=60000)
    parser.add_argument("--server-ip", help="UDP server address; inferred when omitted")
    parser.add_argument("--interface", help="capture interface; inferred when omitted")
    parser.add_argument("--ssh", default="ssh", help="SSH command, including options")
    parser.add_argument("--remote-python", default="python3")
    parser.add_argument(
        "--remote-workload", help="existing path to workload.py on the remote host"
    )
    parser.add_argument("--term", default="xterm-256color")
    parser.add_argument("--mosh-arg", action="append", default=[])
    parser.add_argument(
        "--tcpdump-command",
        help="capture command; default is sudo -n tcpdump when non-root",
    )
    parser.add_argument(
        "--no-capture",
        action="store_true",
        help="exercise the harness without packet capture",
    )
    parser.add_argument("--connect-timeout", type=float, default=45.0)
    parser.add_argument(
        "--run-timeout",
        type=float,
        default=30.0,
        help="extra time allowed beyond workload duration",
    )
    parser.add_argument("--grace", type=float, default=1.0)
    parser.add_argument("--results", type=Path, default=default_results)
    parser.add_argument("--keep-going", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    if args.variant is None:
        args.variant = [
            Variant("patched", ("/usr/local/bin/goblin-mosh",)),
            Variant("system", ("mosh",)),
        ]
    if args.workload is None:
        args.workload = list(DEFAULT_WORKLOADS)
    variant_names = [variant.name for variant in args.variant]
    if len(variant_names) != len(set(variant_names)):
        parser.error("variant names must be unique")
    if len(args.workload) != len(set(args.workload)):
        parser.error("workloads must not be repeated")
    if args.duration <= 0 or args.hz <= 0 or args.repetitions < 1:
        parser.error("duration, hz, and repetitions must be positive")
    if args.width < 40 or args.height < 12:
        parser.error("terminal must be at least 40x12")
    job_count = len(args.variant) * len(args.workload) * args.repetitions
    if args.port_base < 1 or args.port_base + job_count - 1 > 65535:
        parser.error("port range exceeds 1..65535")
    if args.grace < 0 or args.connect_timeout <= 0 or args.run_timeout <= 0:
        parser.error("timeouts must be positive and grace non-negative")
    return args


def command_exists(command: tuple[str, ...]) -> bool:
    executable = command[0]
    return (
        Path(executable).is_file()
        if "/" in executable
        else shutil.which(executable) is not None
    )


def describe_variant(variant: Variant) -> dict[str, Any]:
    executable = variant.command[0]
    resolved = (
        str(Path(executable).resolve())
        if "/" in executable
        else shutil.which(executable)
    )
    description: dict[str, Any] = {
        "name": variant.name,
        "command": list(variant.command),
        "resolved_executable": resolved,
    }
    try:
        completed = subprocess.run(
            list(variant.command) + ["--version"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=5,
            check=False,
        )
        description["version_exit_code"] = completed.returncode
        description["version_output"] = completed.stdout.decode(
            errors="replace"
        ).strip()
    except (OSError, subprocess.TimeoutExpired) as error:
        description["version_error"] = f"{type(error).__name__}: {error}"
    return description


def ssh_argv(ssh_text: str) -> list[str]:
    result = shlex.split(ssh_text)
    if not result:
        raise ValueError("SSH command is empty")
    return result


def deploy_workload(ssh: list[str], host: str, source_path: Path) -> str:
    script = (
        'umask 077; d="${XDG_CACHE_HOME:-$HOME/.cache}/goblin-mosh-ip-volume"; '
        'mkdir -p "$d" || exit 1; cat > "$d/workload.py" || exit 1; '
        'chmod 700 "$d/workload.py" || exit 1; printf "%s\\n" "$d/workload.py"'
    )
    completed = subprocess.run(
        ssh + [host, script],
        input=source_path.read_bytes(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"workload deployment failed:\n{completed.stderr.decode(errors='replace')}"
        )
    remote_path = completed.stdout.decode(errors="strict").strip().splitlines()
    if len(remote_path) != 1 or not remote_path[0].startswith("/"):
        raise RuntimeError(
            f"unexpected workload deployment response: {completed.stdout!r}"
        )
    return remote_path[0]


def resolve_server_ip(host: str, ssh: list[str], explicit: str | None) -> str:
    if explicit is not None:
        return str(ipaddress.ip_address(explicit))

    candidate = host.rsplit("@", 1)[-1].strip("[]")
    config = subprocess.run(
        ssh + ["-G", host],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    if config.returncode == 0:
        for line in config.stdout.decode(errors="replace").splitlines():
            key, _, value = line.partition(" ")
            if key.lower() == "hostname" and value:
                candidate = value.strip()
                break
    try:
        addresses = socket.getaddrinfo(candidate, None, type=socket.SOCK_DGRAM)
        normalized = [str(ipaddress.ip_address(item[4][0])) for item in addresses]
        ipv4 = [
            address
            for address in normalized
            if ipaddress.ip_address(address).version == 4
        ]
        return (ipv4 or normalized)[0]
    except (OSError, ValueError, IndexError):
        pass

    probe = subprocess.run(
        ssh + [host, 'printf "%s\\n" "${SSH_CONNECTION:-}"'],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    fields = probe.stdout.decode(errors="replace").strip().split()
    if probe.returncode == 0 and len(fields) == 4:
        return str(ipaddress.ip_address(fields[2]))
    raise RuntimeError("could not infer UDP server address; pass --server-ip")


def infer_interface(server_ip: str, explicit: str | None) -> str:
    if explicit:
        return explicit
    route = shutil.which("route") or "/sbin/route"
    if Path(route).exists():
        completed = subprocess.run(
            [route, "-n", "get", server_ip],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        match = re.search(
            r"^\s*interface:\s*(\S+)",
            completed.stdout.decode(errors="replace"),
            re.MULTILINE,
        )
        if match:
            return match.group(1)
    ip_command = shutil.which("ip")
    if ip_command:
        completed = subprocess.run(
            [ip_command, "route", "get", server_ip],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        match = re.search(r"\bdev\s+(\S+)", completed.stdout.decode(errors="replace"))
        if match:
            return match.group(1)
    raise RuntimeError("could not infer capture interface; pass --interface")


def capture_command(explicit: str | None) -> list[str]:
    if explicit:
        result = shlex.split(explicit)
        if not result:
            raise ValueError("tcpdump command is empty")
        return result
    tcpdump = shutil.which("tcpdump") or "/usr/sbin/tcpdump"
    if not Path(tcpdump).exists():
        raise RuntimeError("tcpdump not found")
    return [tcpdump] if os.geteuid() == 0 else ["sudo", "-n", tcpdump]


def set_window_size(fd: int, width: int, height: int) -> None:
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", height, width, 0, 0))


def terminate_child(pid: int) -> int:
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        waited, status = os.waitpid(pid, os.WNOHANG)
        if waited == pid:
            return os.waitstatus_to_exitcode(status)
        time.sleep(0.05)
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    _, status = os.waitpid(pid, 0)
    return os.waitstatus_to_exitcode(status)


def run_client(
    command: list[str],
    terminal_log: Path,
    token: str,
    width: int,
    height: int,
    term: str,
    connect_timeout: float,
    run_deadline_seconds: float,
) -> ClientResult:
    environment = os.environ.copy()
    environment["TERM"] = term
    pid, master = pty.fork()
    if pid == 0:
        os.execvpe(command[0], command, environment)
        raise AssertionError("exec returned")

    set_window_size(master, width, height)
    ready_marker = f"{MARKER}_READY:{token}".encode("ascii")
    done_pattern = re.compile(
        rb"MOSH_IP_BENCH_DONE:"
        + re.escape(token.encode("ascii"))
        + rb":(\d+):([A-Za-z0-9_-]{43})"
    )
    ready_ns = 0
    start_ns = 0
    done_ns = 0
    updates = -1
    digest = ""
    visible = VisibleAscii()
    child_status: int | None = None
    master_eof = False
    drain_deadline: float | None = None
    connect_deadline = time.monotonic() + connect_timeout
    run_deadline: float | None = None

    try:
        with terminal_log.open("wb") as output:
            while True:
                readable, _, _ = select.select([master], [], [], 0.1)
                if readable:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError:
                        chunk = b""
                    if chunk:
                        output.write(chunk)
                        visible.feed(chunk)
                        if ready_ns == 0 and ready_marker in visible.data:
                            ready_ns = time.time_ns()
                            start_ns = time.time_ns()
                            os.write(master, b"g")
                            run_deadline = time.monotonic() + run_deadline_seconds
                        if done_ns == 0:
                            match = done_pattern.search(visible.data)
                            if match:
                                done_ns = time.time_ns()
                                updates = int(match.group(1))
                                encoded = match.group(2)
                                digest = base64.urlsafe_b64decode(encoded + b"=").hex()
                    else:
                        master_eof = True

                if child_status is None:
                    waited, status = os.waitpid(pid, os.WNOHANG)
                    if waited == pid:
                        child_status = status
                        drain_deadline = time.monotonic() + 1.0
                now = time.monotonic()
                if child_status is not None and (master_eof or now > drain_deadline):
                    break
                if child_status is None and ready_ns == 0 and now > connect_deadline:
                    raise TimeoutError(
                        f"client did not receive READY marker within {connect_timeout:.1f}s"
                    )
                if (
                    child_status is None
                    and run_deadline is not None
                    and now > run_deadline
                ):
                    raise TimeoutError(
                        "client did not finish workload before run timeout"
                    )
    except BaseException:
        if child_status is None:
            terminate_child(pid)
        raise
    finally:
        os.close(master)

    exit_ns = time.time_ns()
    assert child_status is not None
    exit_code = os.waitstatus_to_exitcode(child_status)
    if ready_ns == 0:
        raise RuntimeError(f"client exited with {exit_code} before READY marker")
    if done_ns == 0:
        raise RuntimeError(f"client exited with {exit_code} before DONE marker")
    return ClientResult(
        exit_code, ready_ns, start_ns, done_ns, exit_ns, updates, digest
    )


def mosh_command(
    job: Job,
    args: argparse.Namespace,
    ssh: list[str],
    remote_workload: str,
    token: str,
    server_ip: str,
) -> list[str]:
    family = "inet6" if ipaddress.ip_address(server_ip).version == 6 else "inet"
    options = [
        "--predict=never",
        "--no-init",
        "--no-ssh-pty",
        f"--family={family}",
        f"--port={job.port}",
        f"--ssh={shlex.join(ssh)}",
    ]
    options.extend(args.mosh_arg)
    remote = [
        args.remote_python,
        remote_workload,
        "--workload",
        job.workload,
        "--duration",
        str(args.duration),
        "--hz",
        str(args.hz),
        "--seed",
        str(job.seed),
        "--width",
        str(args.width),
        "--height",
        str(args.height),
        "--token",
        token,
        "--grace",
        str(args.grace),
    ]
    return list(job.variant.command) + options + ["--", args.host] + remote


def build_jobs(args: argparse.Namespace) -> list[Job]:
    cases = []
    for workload_index, workload in enumerate(args.workload):
        for repetition in range(1, args.repetitions + 1):
            seed = args.seed + workload_index * 1000 + repetition - 1
            cases.append((workload, repetition, seed))

    randomizer = random.Random(args.order_seed)
    randomizer.shuffle(cases)
    jobs: list[Job] = []
    port = args.port_base
    for workload, repetition, seed in cases:
        variants = list(args.variant)
        randomizer.shuffle(variants)
        for variant in variants:
            jobs.append(Job(variant, workload, repetition, seed, port))
            port += 1
    return jobs


def flatten_stats(
    prefix: str, stats: pcap_ip.CaptureStats | None, record: dict[str, Any]
) -> None:
    for direction in ("server_to_client", "client_to_server", "unrelated"):
        values = getattr(stats, direction) if stats is not None else None
        record[f"{prefix}_{direction}_packets"] = (
            values.packets if values is not None else None
        )
        record[f"{prefix}_{direction}_ip_bytes"] = (
            values.ip_bytes if values is not None else None
        )


def format_bytes(value: float) -> str:
    if value < 1024:
        return f"{value:.0f} B"
    if value < 1024 * 1024:
        return f"{value / 1024:.1f} KiB"
    return f"{value / (1024 * 1024):.2f} MiB"


def print_summary(records: list[dict[str, Any]]) -> None:
    successful = [
        record
        for record in records
        if record.get("error") is None
        and record.get("active_server_to_client_ip_bytes") is not None
    ]
    if not successful:
        print("\nNo captured successful runs to summarize.")
        return
    print("\nMedian active-window server -> client IP volume")
    print(
        f"{'workload':22} {'variant':14} {'runs':>4} {'IP bytes':>12} {'packets':>10}"
    )
    medians: dict[tuple[str, str], float] = {}
    for workload in sorted({record["workload"] for record in successful}):
        for variant in sorted(
            {
                record["variant"]
                for record in successful
                if record["workload"] == workload
            }
        ):
            group = [
                record
                for record in successful
                if record["workload"] == workload and record["variant"] == variant
            ]
            byte_median = statistics.median(
                record["active_server_to_client_ip_bytes"] for record in group
            )
            packet_median = statistics.median(
                record["active_server_to_client_packets"] for record in group
            )
            medians[(workload, variant)] = byte_median
            print(
                f"{workload:22} {variant:14} {len(group):4d} {format_bytes(byte_median):>12} {packet_median:10.1f}"
            )

    if {"patched", "system"}.issubset({record["variant"] for record in successful}):
        print("\nMedian paired downlink savings (negative means more bytes)")
        for workload in sorted({record["workload"] for record in successful}):
            cases: dict[tuple[int, int], dict[str, int]] = {}
            for record in successful:
                if record["workload"] != workload:
                    continue
                key = (record["repetition"], record["seed"])
                cases.setdefault(key, {})[record["variant"]] = record[
                    "active_server_to_client_ip_bytes"
                ]
            savings = [
                100.0 * (1.0 - values["patched"] / values["system"])
                for values in cases.values()
                if "patched" in values and "system" in values and values["system"] > 0
            ]
            if savings:
                print(
                    f"{workload:22} {statistics.median(savings):8.2f}% "
                    f"({len(savings)} pairs)"
                )


def write_csv(path: Path, records: list[dict[str, Any]]) -> None:
    if not records:
        return
    fields = sorted({key for record in records for key in record})
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(records)


def verify_workload_output(
    expected: dict[tuple[str, int, int], tuple[int, str, str]],
    job: Job,
    updates: int,
    digest: str,
) -> None:
    key = (job.workload, job.repetition, job.seed)
    signature = (updates, digest)
    previous = expected.get(key)
    if previous is None:
        expected[key] = (updates, digest, job.variant.name)
        return
    previous_updates, previous_digest, previous_variant = previous
    if signature != (previous_updates, previous_digest):
        raise RuntimeError(
            "workload output differs from matched run: "
            f"{job.variant.name} reported updates={updates} sha256={digest}, "
            f"but {previous_variant} reported updates={previous_updates} sha256={previous_digest}"
        )


def main() -> int:
    args = parse_args()
    for variant in args.variant:
        if not command_exists(variant.command):
            raise SystemExit(f"variant executable not found: {variant.command[0]}")
    ssh = ssh_argv(args.ssh)
    jobs = build_jobs(args)
    if args.dry_run:
        remote_workload = args.remote_workload or "/REMOTE/PATH/workload.py"
        server_ip = args.server_ip or "192.0.2.1"
        for job in jobs:
            token = "DRYRUN"
            print(
                shlex.join(
                    mosh_command(job, args, ssh, remote_workload, token, server_ip)
                )
            )
        return 0

    server_ip = resolve_server_ip(args.host, ssh, args.server_ip)
    interface = (
        "none" if args.no_capture else infer_interface(server_ip, args.interface)
    )
    tcpdump = [] if args.no_capture else capture_command(args.tcpdump_command)
    remote_workload = args.remote_workload or deploy_workload(
        ssh, args.host, Path(__file__).resolve().with_name("workload.py")
    )

    args.results.mkdir(parents=True, exist_ok=False)
    workload_source = Path(__file__).resolve().with_name("workload.py").read_bytes()
    runner_source = Path(__file__).resolve().read_bytes()
    metadata = {
        "started_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "host": args.host,
        "server_ip": server_ip,
        "interface": interface,
        "ssh_command": ssh,
        "capture_command": tcpdump or None,
        "variants": [describe_variant(variant) for variant in args.variant],
        "workloads": args.workload,
        "duration": args.duration,
        "hz": args.hz,
        "repetitions": args.repetitions,
        "seed": args.seed,
        "port_base": args.port_base,
        "width": args.width,
        "height": args.height,
        "term": args.term,
        "mosh_args": args.mosh_arg,
        "connect_timeout": args.connect_timeout,
        "run_timeout": args.run_timeout,
        "grace": args.grace,
        "remote_workload": remote_workload,
        "workload_sha256": hashlib.sha256(workload_source).hexdigest(),
        "runner_sha256": hashlib.sha256(runner_source).hexdigest(),
        "order_seed": args.order_seed,
        "local_platform": platform.platform(),
        "local_hostname": socket.gethostname(),
        "local_python": sys.version,
    }
    (args.results / "metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8"
    )
    records: list[dict[str, Any]] = []
    expected_outputs: dict[tuple[str, int, int], tuple[int, str, str]] = {}
    jsonl_path = args.results / "runs.jsonl"

    print(f"server={server_ip} interface={interface} results={args.results}")
    if "kitty-place" in args.workload:
        print(
            "note: kitty-place is a feature benchmark; validate rendered output because stock Mosh may discard it"
        )

    with jsonl_path.open("a", encoding="utf-8") as jsonl:
        for index, job in enumerate(jobs, 1):
            stem = f"{index:03d}-{job.workload}-{job.variant.name}-r{job.repetition}"
            pcap_path = args.results / f"{stem}.pcap"
            terminal_log = args.results / f"{stem}.terminal"
            capture_log = args.results / f"{stem}.tcpdump.log"
            token = uuid.uuid4().hex[:12]
            command = mosh_command(job, args, ssh, remote_workload, token, server_ip)
            print(
                f"[{index}/{len(jobs)}] {job.workload} {job.variant.name} "
                f"repeat={job.repetition} seed={job.seed} port={job.port}"
            )
            record: dict[str, Any] = {
                "variant": job.variant.name,
                "workload": job.workload,
                "repetition": job.repetition,
                "seed": job.seed,
                "port": job.port,
                "command": command,
                "pcap": str(pcap_path) if not args.no_capture else None,
                "terminal_log": str(terminal_log),
                "error": None,
            }
            capture: Capture | None = None
            try:
                if not args.no_capture:
                    capture = Capture(
                        tcpdump, interface, server_ip, job.port, pcap_path, capture_log
                    )
                client = run_client(
                    command,
                    terminal_log,
                    token,
                    args.width,
                    args.height,
                    args.term,
                    args.connect_timeout,
                    args.duration + args.grace + args.run_timeout,
                )
                if client.exit_code != 0:
                    raise RuntimeError(f"client exited with status {client.exit_code}")
                if capture is not None:
                    completed_capture = capture
                    capture = None
                    completed_capture.stop()
                captured = (
                    list(pcap_ip.packets(pcap_path)) if not args.no_capture else []
                )
                active = (
                    pcap_ip.summarize(
                        captured, server_ip, client.start_ns, client.done_ns
                    )
                    if not args.no_capture
                    else None
                )
                session = (
                    pcap_ip.summarize(captured, server_ip)
                    if not args.no_capture
                    else None
                )
                record.update(
                    {
                        "exit_code": client.exit_code,
                        "ready_ns": client.ready_ns,
                        "start_ns": client.start_ns,
                        "done_ns": client.done_ns,
                        "exit_ns": client.exit_ns,
                        "active_seconds": (client.done_ns - client.start_ns)
                        / 1_000_000_000,
                        "session_seconds": (client.exit_ns - client.ready_ns)
                        / 1_000_000_000,
                        "updates": client.updates,
                        "workload_sha256": client.digest,
                    }
                )
                flatten_stats("active", active, record)
                flatten_stats("session", session, record)
                verify_workload_output(
                    expected_outputs, job, client.updates, client.digest
                )
                if not args.no_capture and not captured:
                    raise RuntimeError(
                        "packet capture contained no IP packets; check --server-ip and --interface"
                    )
                if active is not None and active.server_to_client.packets == 0:
                    raise RuntimeError(
                        "packet capture contained no active-window downlink packets; "
                        "check --server-ip and --interface"
                    )
                if active is not None:
                    print(
                        f"    down={format_bytes(active.server_to_client.ip_bytes)} "
                        f"({active.server_to_client.packets} packets) "
                        f"up={format_bytes(active.client_to_server.ip_bytes)}"
                    )
            except BaseException as error:
                if capture is not None:
                    try:
                        capture.stop()
                    except BaseException as capture_error:
                        error = RuntimeError(
                            f"{error}; additionally failed to stop capture: {capture_error}"
                        )
                record["error"] = f"{type(error).__name__}: {error}"
                print(f"    ERROR: {record['error']}", file=sys.stderr)
                if not args.keep_going:
                    records.append(record)
                    jsonl.write(json.dumps(record, sort_keys=True) + "\n")
                    jsonl.flush()
                    write_csv(args.results / "runs.csv", records)
                    return 1
            records.append(record)
            jsonl.write(json.dumps(record, sort_keys=True) + "\n")
            jsonl.flush()

    write_csv(args.results / "runs.csv", records)
    print_summary(records)
    return 1 if any(record.get("error") for record in records) else 0


if __name__ == "__main__":
    raise SystemExit(main())
