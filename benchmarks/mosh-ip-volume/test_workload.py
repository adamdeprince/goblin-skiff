#!/usr/bin/env python3

from __future__ import annotations

import re
import subprocess
import sys
import unittest
from pathlib import Path


WORKLOAD = Path(__file__).resolve().with_name("workload.py")
NAMES = (
    "idle",
    "dashboard",
    "scroll-repetitive",
    "scroll-entropy",
    "full-redraw",
    "kitty-place",
)


def invoke(name: str, token: str) -> tuple[int, str, bytes]:
    completed = subprocess.run(
        [
            sys.executable,
            str(WORKLOAD),
            "--workload",
            name,
            "--duration",
            "0.001",
            "--hz",
            "1000",
            "--seed",
            "17",
            "--width",
            "80",
            "--height",
            "24",
            "--token",
            token,
            "--grace",
            "0",
        ],
        input=b"g",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=5,
        check=False,
    )
    if completed.returncode != 0:
        raise AssertionError(completed.stderr.decode(errors="replace"))
    ready = f"MOSH_IP_BENCH_READY:{token}".encode()
    if ready not in completed.stdout:
        raise AssertionError(f"missing READY marker in {completed.stdout!r}")
    match = re.search(
        rb"MOSH_IP_BENCH_DONE:"
        + re.escape(token.encode())
        + rb":(\d+):([A-Za-z0-9_-]{43})",
        completed.stdout,
    )
    if match is None:
        raise AssertionError(f"missing DONE marker in {completed.stdout!r}")
    return int(match.group(1)), match.group(2).decode(), completed.stdout


class WorkloadTest(unittest.TestCase):
    def test_every_workload_is_deterministic(self) -> None:
        for name in NAMES:
            with self.subTest(workload=name):
                first_updates, first_digest, _ = invoke(name, "first")
                second_updates, second_digest, _ = invoke(name, "second")
                self.assertEqual(first_updates, second_updates)
                self.assertEqual(first_digest, second_digest)
                self.assertEqual(0 if name == "idle" else 1, first_updates)

    def test_closed_input_fails_instead_of_waiting_forever(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(WORKLOAD), "--workload", "idle", "--token", "closed"],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
            check=False,
        )
        self.assertNotEqual(0, completed.returncode)
        self.assertIn(b"input closed before start signal", completed.stderr)


if __name__ == "__main__":
    unittest.main()
