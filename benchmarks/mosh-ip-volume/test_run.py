#!/usr/bin/env python3

from __future__ import annotations

import unittest
from types import SimpleNamespace

import run as benchmark


class RunTest(unittest.TestCase):
    def test_jobs_keep_randomized_variants_in_matched_blocks(self) -> None:
        variants = [
            benchmark.Variant("patched", ("true",)),
            benchmark.Variant("system", ("true",)),
        ]
        args = SimpleNamespace(
            workload=["idle", "dashboard"],
            repetitions=3,
            seed=7,
            order_seed=11,
            variant=variants,
            port_base=60000,
        )
        jobs = benchmark.build_jobs(args)
        self.assertEqual(list(range(60000, 60012)), [job.port for job in jobs])
        for offset in range(0, len(jobs), 2):
            block = jobs[offset : offset + 2]
            self.assertEqual(
                1, len({(job.workload, job.repetition, job.seed) for job in block})
            )
            self.assertEqual({"patched", "system"}, {job.variant.name for job in block})

    def test_visible_ascii_joins_text_split_by_terminal_controls(self) -> None:
        visible = benchmark.VisibleAscii()
        visible.feed(b"MOSH_IP_BENCH_DONE:abc123:1:abcdefghij")
        visible.feed(b"\x1b[11;80HklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ")
        visible.feed(b"RSTUVWXYZ0123456\x1b]0;ignored title\x07")
        self.assertIn(
            b"MOSH_IP_BENCH_DONE:abc123:1:abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQ"
            b"RSTUVWXYZ0123456",
            visible.data,
        )

    def test_matched_workloads_must_have_identical_output(self) -> None:
        expected: dict[tuple[str, int, int], tuple[int, str, str]] = {}
        patched = benchmark.Job(
            benchmark.Variant("patched", ("true",)), "dashboard", 1, 7, 60000
        )
        system = benchmark.Job(
            benchmark.Variant("system", ("true",)), "dashboard", 1, 7, 60001
        )
        benchmark.verify_workload_output(expected, patched, 100, "a" * 64)
        benchmark.verify_workload_output(expected, system, 100, "a" * 64)
        with self.assertRaisesRegex(RuntimeError, "workload output differs"):
            benchmark.verify_workload_output(expected, system, 100, "b" * 64)


if __name__ == "__main__":
    unittest.main()
