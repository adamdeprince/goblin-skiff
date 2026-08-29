#!/usr/bin/env python3

from __future__ import annotations

import ipaddress
import struct
import tempfile
import unittest
from pathlib import Path

import pcap_ip


def ethernet(payload: bytes, ether_type: int) -> bytes:
    return b"\0" * 12 + struct.pack("!H", ether_type) + payload


def ipv4(source: str, destination: str, payload_size: int) -> bytes:
    total_length = 20 + payload_size
    return (
        bytes((0x45, 0))
        + struct.pack("!H", total_length)
        + b"\0\0\0\0\x40\x11\0\0"
        + ipaddress.IPv4Address(source).packed
        + ipaddress.IPv4Address(destination).packed
        + b"x" * payload_size
    )


def ipv6(source: str, destination: str, payload_size: int) -> bytes:
    return (
        b"\x60\0\0\0"
        + struct.pack("!H", payload_size)
        + b"\x11\x40"
        + ipaddress.IPv6Address(source).packed
        + ipaddress.IPv6Address(destination).packed
        + b"y" * payload_size
    )


def write_pcap(path: Path, frames: list[tuple[int, bytes]]) -> None:
    with path.open("wb") as stream:
        stream.write(b"\xd4\xc3\xb2\xa1")
        stream.write(struct.pack("<HHIIII", 2, 4, 0, 0, 65535, 1))
        for timestamp_us, frame in frames:
            seconds, micros = divmod(timestamp_us, 1_000_000)
            stream.write(struct.pack("<IIII", seconds, micros, len(frame), len(frame)))
            stream.write(frame)


class PcapIpTest(unittest.TestCase):
    def test_counts_ip_bytes_by_direction_and_window(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.pcap"
            write_pcap(
                path,
                [
                    (1_000_000, ethernet(ipv4("192.0.2.10", "192.0.2.20", 80), 0x0800)),
                    (2_000_000, ethernet(ipv4("192.0.2.20", "192.0.2.10", 40), 0x0800)),
                    (
                        3_000_000,
                        ethernet(ipv6("2001:db8::1", "2001:db8::2", 60), 0x86DD),
                    ),
                ],
            )
            captured = list(pcap_ip.packets(path))
            self.assertEqual([100, 60, 100], [packet.ip_bytes for packet in captured])

            stats = pcap_ip.summarize(captured, "192.0.2.10")
            self.assertEqual(1, stats.server_to_client.packets)
            self.assertEqual(100, stats.server_to_client.ip_bytes)
            self.assertEqual(1, stats.client_to_server.packets)
            self.assertEqual(60, stats.client_to_server.ip_bytes)
            self.assertEqual(1, stats.unrelated.packets)

            window = pcap_ip.summarize(
                captured, "192.0.2.10", 1_500_000_000, 2_500_000_000
            )
            self.assertEqual(0, window.server_to_client.packets)
            self.assertEqual(60, window.client_to_server.ip_bytes)

    def test_vlan_frame(self) -> None:
        packet = ipv4("198.51.100.4", "198.51.100.5", 8)
        frame = b"\0" * 12 + b"\x81\x00\0\x01\x08\x00" + packet
        parsed = pcap_ip.parse_frame(frame, 1, 7)
        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(28, parsed.ip_bytes)
        self.assertEqual("198.51.100.4", parsed.source)


if __name__ == "__main__":
    unittest.main()
