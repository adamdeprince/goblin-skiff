#!/usr/bin/env python3

"""Deterministic terminal workloads for the Mosh IP-volume benchmark."""

from __future__ import annotations

import argparse
import base64
import hashlib
import os
import sys
import termios
import time
from dataclasses import dataclass
from typing import Callable, Protocol


MARKER = "MOSH_IP_BENCH"
WORKLOADS = (
    "idle",
    "dashboard",
    "scroll-repetitive",
    "scroll-entropy",
    "full-redraw",
    "kitty-place",
)


class Digest(Protocol):
    def update(self, data: bytes) -> None: ...

    def hexdigest(self) -> str: ...


class Lcg:
    """Small cross-version deterministic generator."""

    def __init__(self, seed: int) -> None:
        self.state = seed & 0xFFFFFFFF

    def next(self) -> int:
        self.state = (1664525 * self.state + 1013904223) & 0xFFFFFFFF
        return self.state


@dataclass
class Writer:
    digest: Digest

    def write(self, data: bytes) -> None:
        self.digest.update(data)
        write_all(sys.stdout.fileno(), data)


def write_all(fd: int, data: bytes) -> None:
    offset = 0
    while offset < len(data):
        written = os.write(fd, data[offset:])
        if written == 0:
            raise BrokenPipeError("zero-length write")
        offset += written


def marker(text: str) -> None:
    write_all(sys.stdout.fileno(), (f"\r\n{text}\r\n").encode("ascii"))


def wait_for_go(token: str) -> None:
    fd = sys.stdin.fileno()
    saved = None
    if os.isatty(fd):
        saved = termios.tcgetattr(fd)
        current = termios.tcgetattr(fd)
        current[3] &= ~(termios.ECHO | termios.ICANON)
        current[6][termios.VMIN] = 1
        current[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, current)

    marker(f"{MARKER}_READY:{token}")
    try:
        while True:
            byte = os.read(fd, 1)
            if not byte:
                raise EOFError("input closed before start signal")
            if byte == b"g":
                return
    finally:
        if saved is not None:
            termios.tcsetattr(fd, termios.TCSANOW, saved)


def frame_count(duration: float, hz: float) -> int:
    return max(1, int(round(duration * hz)))


def run_timed_frames(count: int, hz: float, render: Callable[[int], None]) -> None:
    start = time.monotonic()
    for index in range(count):
        render(index)
        deadline = start + (index + 1) / hz
        delay = deadline - time.monotonic()
        if delay > 0:
            time.sleep(delay)


def fit(text: str, width: int) -> str:
    return text[:width].ljust(width)


def dashboard(writer: Writer, args: argparse.Namespace) -> int:
    width = max(40, args.width - 2)
    static = [
        "LOW-BANDWIDTH LINK INSTRUMENT",
        "",
        "channel       state       value          budget",
        "terminal      active      000000         primary",
        "agent         waiting     000000         medium",
        "tcp           waiting     000000         low",
        "bulk          deferred    000000         idle",
        "",
        "loss          00.00%      rtt 0000 ms",
        "progress      [--------------------------------]",
    ]
    writer.write(b"\x1b[2J\x1b[H\x1b[?25l")
    writer.write(("\r\n".join(fit(line, width) for line in static)).encode("ascii"))

    count = frame_count(args.duration, args.hz)

    def render(index: int) -> None:
        step = index + 1
        values = (
            (step * 37 + args.seed) % 1000000,
            (step * 11 + args.seed * 3) % 1000000,
            (step * 19 + args.seed * 5) % 1000000,
            (step * 7 + args.seed * 13) % 1000000,
        )
        loss = ((step * 17 + args.seed) % 500) / 100.0
        rtt = 480 + ((step * 29 + args.seed) % 240)
        filled = (step * 32) // count
        bar = "#" * filled + "-" * (32 - filled)
        update = (
            f"\x1b[4;27H{values[0]:06d}"
            f"\x1b[5;27H{values[1]:06d}"
            f"\x1b[6;27H{values[2]:06d}"
            f"\x1b[7;27H{values[3]:06d}"
            f"\x1b[9;15H{loss:05.2f}%"
            f"\x1b[9;31H{rtt:04d} ms"
            f"\x1b[10;16H[{bar}]"
        )
        writer.write(update.encode("ascii"))

    run_timed_frames(count, args.hz, render)
    return count


def scroll_repetitive(writer: Writer, args: argparse.Namespace) -> int:
    writer.write(b"\x1b[2J\x1b[H\x1b[?25l")
    count = frame_count(args.duration, args.hz)
    components = ("terminal", "scheduler", "forward", "fec", "state")

    def render(index: int) -> None:
        component = components[index % len(components)]
        value = (args.seed + index * 37) % 100000
        line = (
            f"seq={index:06d} component={component:<9} status=ok "
            f"value={value:05d} queue={(index * 3) % 64:02d}\n"
        )
        writer.write(line.encode("ascii"))

    run_timed_frames(count, args.hz, render)
    return count


def scroll_entropy(writer: Writer, args: argparse.Namespace) -> int:
    writer.write(b"\x1b[2J\x1b[H\x1b[?25l")
    count = frame_count(args.duration, args.hz)
    generator = Lcg(args.seed)
    alphabet = b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    line_width = max(32, args.width - 2)

    def render(index: int) -> None:
        del index
        line = bytes(
            alphabet[generator.next() % len(alphabet)] for _ in range(line_width)
        )
        writer.write(line + b"\n")

    run_timed_frames(count, args.hz, render)
    return count


def full_redraw(writer: Writer, args: argparse.Namespace) -> int:
    writer.write(b"\x1b[2J\x1b[H\x1b[?25l")
    count = frame_count(args.duration, args.hz)
    width = max(20, args.width - 1)
    rows = max(4, args.height - 1)

    def screen(phase: int) -> bytes:
        fill_char = "." if phase == 0 else ":"
        lines = []
        for row in range(rows):
            label = f"frame={phase} row={row:02d} seed={args.seed:08x} "
            tail = fill_char * max(0, width - len(label))
            lines.append((label + tail)[:width])
        return ("\x1b[H" + "\r\n".join(lines)).encode("ascii")

    screens = (screen(0), screen(1))
    run_timed_frames(count, args.hz, lambda index: writer.write(screens[index & 1]))
    return count


def kitty_command(controls: str, payload: bytes = b"") -> bytes:
    encoded = base64.b64encode(payload)
    separator = b";" if encoded else b""
    return b"\x1b_G" + controls.encode("ascii") + separator + encoded + b"\x1b\\"


def kitty_place(writer: Writer, args: argparse.Namespace) -> int:
    writer.write(b"\x1b[2J\x1b[H\x1b[?25l")
    image_width = 32
    image_height = 16
    pixels = bytearray()
    for y in range(image_height):
        for x in range(image_width):
            pixels.extend(
                (
                    (x * 7 + args.seed) & 0xFF,
                    (y * 13 + args.seed) & 0xFF,
                    ((x ^ y) * 17) & 0xFF,
                )
            )
    writer.write(
        kitty_command(
            f"a=T,f=24,s={image_width},v={image_height},i=1,p=1,C=1,q=2", bytes(pixels)
        )
    )

    count = frame_count(args.duration, args.hz)
    max_row = max(1, args.height - 6)
    max_col = max(1, args.width - 12)

    def render(index: int) -> None:
        row = 1 + (index * 3) % max_row
        col = 1 + (index * 7) % max_col
        commands = kitty_command("a=d,d=p,i=1,p=1,q=2")
        commands += f"\x1b[{row};{col}H".encode("ascii")
        commands += kitty_command("a=p,i=1,p=1,C=1,q=2")
        writer.write(commands)

    run_timed_frames(count, args.hz, render)
    return count


def idle(writer: Writer, args: argparse.Namespace) -> int:
    writer.write(b"\x1b[2J\x1b[H\x1b[?25l")
    time.sleep(args.duration)
    return 0


RUNNERS = {
    "idle": idle,
    "dashboard": dashboard,
    "scroll-repetitive": scroll_repetitive,
    "scroll-entropy": scroll_entropy,
    "full-redraw": full_redraw,
    "kitty-place": kitty_place,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workload", choices=WORKLOADS, required=True)
    parser.add_argument("--duration", type=float, default=10.0)
    parser.add_argument("--hz", type=float, default=10.0)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--width", type=int, default=80)
    parser.add_argument("--height", type=int, default=24)
    parser.add_argument("--token", required=True)
    parser.add_argument("--grace", type=float, default=1.0)
    args = parser.parse_args()
    if args.duration <= 0 or args.hz <= 0 or args.grace < 0:
        parser.error("duration and hz must be positive; grace must be non-negative")
    if args.width < 40 or args.height < 12:
        parser.error("terminal must be at least 40x12")
    return args


def main() -> int:
    args = parse_args()
    write_all(sys.stdout.fileno(), b"\x1b[2J\x1b[H")
    wait_for_go(args.token)
    digest = hashlib.sha256()
    writer = Writer(digest)
    try:
        updates = RUNNERS[args.workload](writer, args)
        writer.write(b"\x1b[0m\x1b[?25h")
        encoded_digest = (
            base64.urlsafe_b64encode(digest.digest()).rstrip(b"=").decode("ascii")
        )
        marker(f"{MARKER}_DONE:{args.token}:{updates}:{encoded_digest}")
        time.sleep(args.grace)
    finally:
        write_all(sys.stdout.fileno(), b"\x1b[0m\x1b[?25h")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
