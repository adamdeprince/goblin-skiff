#!/usr/bin/env python3

"""Read tcpdump pcap files and account for captured IPv4/IPv6 packet bytes."""

from __future__ import annotations

import ipaddress
import struct
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Iterator


@dataclass(frozen=True)
class IpPacket:
    timestamp_ns: int
    source: str
    destination: str
    ip_bytes: int
    version: int


@dataclass
class DirectionStats:
    packets: int = 0
    ip_bytes: int = 0

    def add(self, packet: IpPacket) -> None:
        self.packets += 1
        self.ip_bytes += packet.ip_bytes


@dataclass
class CaptureStats:
    server_to_client: DirectionStats
    client_to_server: DirectionStats
    unrelated: DirectionStats

    def to_dict(self) -> dict[str, dict[str, int]]:
        return asdict(self)


PCAP_MAGICS = {
    b"\xd4\xc3\xb2\xa1": ("<", 1_000),
    b"\xa1\xb2\xc3\xd4": (">", 1_000),
    b"\x4d\x3c\xb2\xa1": ("<", 1),
    b"\xa1\xb2\x3c\x4d": (">", 1),
}


def packets(path: Path | str) -> Iterator[IpPacket]:
    with Path(path).open("rb") as stream:
        global_header = stream.read(24)
        if len(global_header) != 24 or global_header[:4] not in PCAP_MAGICS:
            raise ValueError(f"{path}: not a classic pcap file")
        endian, fraction_ns = PCAP_MAGICS[global_header[:4]]
        _, _, _, _, _, link_type = struct.unpack(endian + "HHIIII", global_header[4:])

        while True:
            packet_header = stream.read(16)
            if not packet_header:
                return
            if len(packet_header) != 16:
                raise ValueError(f"{path}: truncated pcap packet header")
            seconds, fraction, captured_length, _ = struct.unpack(
                endian + "IIII", packet_header
            )
            frame = stream.read(captured_length)
            if len(frame) != captured_length:
                raise ValueError(f"{path}: truncated pcap packet data")
            packet = parse_frame(
                frame, link_type, seconds * 1_000_000_000 + fraction * fraction_ns
            )
            if packet is not None:
                yield packet


def network_offset(frame: bytes, link_type: int) -> int | None:
    if link_type == 1:  # LINKTYPE_ETHERNET
        if len(frame) < 14:
            return None
        offset = 14
        ether_type = struct.unpack("!H", frame[12:14])[0]
        while ether_type in (0x8100, 0x88A8, 0x9100):
            if len(frame) < offset + 4:
                return None
            ether_type = struct.unpack("!H", frame[offset + 2 : offset + 4])[0]
            offset += 4
        return offset
    if link_type in (0, 108):  # BSD null/loopback
        return 4
    if link_type == 113:  # Linux cooked capture v1
        return 16
    if link_type == 276:  # Linux cooked capture v2
        return 20
    if link_type in (12, 101, 228, 229):  # Raw IP, explicit IPv4, explicit IPv6
        return 0
    raise ValueError(
        f"unsupported pcap link type {link_type}; capture a concrete interface instead of an aggregate/pktap device"
    )


def parse_frame(frame: bytes, link_type: int, timestamp_ns: int) -> IpPacket | None:
    offset = network_offset(frame, link_type)
    if offset is None or len(frame) <= offset:
        return None
    version = frame[offset] >> 4
    if version == 4:
        if len(frame) < offset + 20:
            return None
        header_length = (frame[offset] & 0x0F) * 4
        total_length = struct.unpack("!H", frame[offset + 2 : offset + 4])[0]
        if header_length < 20 or total_length < header_length:
            return None
        source = str(ipaddress.IPv4Address(frame[offset + 12 : offset + 16]))
        destination = str(ipaddress.IPv4Address(frame[offset + 16 : offset + 20]))
        return IpPacket(timestamp_ns, source, destination, total_length, 4)
    if version == 6:
        if len(frame) < offset + 40:
            return None
        payload_length = struct.unpack("!H", frame[offset + 4 : offset + 6])[0]
        source = str(ipaddress.IPv6Address(frame[offset + 8 : offset + 24]))
        destination = str(ipaddress.IPv6Address(frame[offset + 24 : offset + 40]))
        return IpPacket(timestamp_ns, source, destination, 40 + payload_length, 6)
    return None


def summarize(
    captured: Iterable[IpPacket],
    server_ip: str,
    start_ns: int | None = None,
    end_ns: int | None = None,
) -> CaptureStats:
    server = str(ipaddress.ip_address(server_ip))
    result = CaptureStats(DirectionStats(), DirectionStats(), DirectionStats())
    for packet in captured:
        if start_ns is not None and packet.timestamp_ns < start_ns:
            continue
        if end_ns is not None and packet.timestamp_ns > end_ns:
            continue
        if packet.source == server:
            result.server_to_client.add(packet)
        elif packet.destination == server:
            result.client_to_server.add(packet)
        else:
            result.unrelated.add(packet)
    return result
