"""Loopback-only SOCKS5 fixture. No TUN, tailnet credentials or system DNS changes."""
import contextlib
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import threading
import time


def exact(sock, length):
    data = b""
    while len(data) < length:
        part = sock.recv(length - len(data))
        if not part:
            raise EOFError()
        data += part
    return data


def address(sock):
    kind = exact(sock, 1)[0]
    if kind == 1:
        host = socket.inet_ntop(socket.AF_INET, exact(sock, 4))
    elif kind == 4:
        host = socket.inet_ntop(socket.AF_INET6, exact(sock, 16))
    elif kind == 3:
        host = exact(sock, exact(sock, 1)[0]).decode("ascii")
    else:
        raise AssertionError("invalid SOCKS address")
    return host, struct.unpack("!H", exact(sock, 2))[0]


class Packet:
    def __init__(self, data):
        self.data = data

    def recv(self, length):
        result, self.data = self.data[:length], self.data[length:]
        return result


class Proxy:
    def __init__(self, ipv6=False, restart=False, reject=False, malformed=False, wildcard=False, auth=False, loss=True, noisy=True, udp_only=False):
        self.family = socket.AF_INET6 if ipv6 else socket.AF_INET
        self.host = "::1" if ipv6 else "127.0.0.1"
        self.listener = socket.socket(self.family)
        self.listener.bind((self.host, 0))
        self.listener.listen()
        self.listener.settimeout(0.1)
        self.endpoint = ("[::1]:" if ipv6 else "127.0.0.1:") + str(self.listener.getsockname()[1])
        self.restart, self.reject, self.malformed, self.wildcard, self.auth = restart, reject, malformed, wildcard, auth
        self.loss, self.noisy = loss, noisy
        self.udp_only = udp_only
        self.stop = threading.Event()
        self.threads = []
        self.errors = []
        self.requests = []
        self.associations = 0
        self.forwarded = 0
        self.worker = threading.Thread(target=self.accept, daemon=True)
        self.worker.start()

    def accept(self):
        while not self.stop.is_set():
            try:
                conn, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            worker = threading.Thread(target=self.handle, args=(conn,), daemon=True)
            self.threads.append(worker)
            worker.start()

    def fragmented(self, conn, data):
        for byte in data:
            conn.sendall(bytes([byte]))
            time.sleep(0.001)

    def handle(self, conn):
        try:
            with conn:
                conn.settimeout(3)
                assert exact(conn, 3) == b"\x05\x01\x00"
                self.fragmented(conn, b"\x05\xff" if self.auth else b"\x05\x00")
                if self.auth:
                    return
                ver, command, reserved = exact(conn, 3)
                assert ver == 5 and reserved == 0
                host, port = address(conn)
                self.requests.append((command, host, port))
                if command == 1:
                    # Deliberately unrelated BND.ADDR: never advertise it as the Mosh peer.
                    self.fragmented(conn, b"\x05\x00\x00\x01\xc0\x00\x02\x63\x12\x34")
                    conn.sendall(b"SSH-through-SOCKS\n")
                    data = conn.recv(65536)
                    if data:
                        conn.sendall(data)
                    return
                assert command == 3
                if self.reject:
                    conn.sendall(b"\x05\x07\x00\x01" + bytes(6))
                    return
                if self.malformed:
                    conn.sendall(b"\x05\x00\x01\x01" + bytes(6))
                    return
                with socket.socket(self.family, socket.SOCK_DGRAM) as udp:
                    udp.bind((self.host, 0))
                    relay_port = udp.getsockname()[1]
                    packed = socket.inet_pton(self.family, self.host)
                    if self.wildcard:
                        packed = bytes(len(packed))
                    reply = b"\x05\x00\x00" + bytes([4 if self.family == socket.AF_INET6 else 1]) + packed + struct.pack("!H", relay_port)
                    self.fragmented(conn, reply)
                    self.associations += 1
                    association = self.associations
                    self.udp_loop(conn, udp, association)
        except (EOFError, ConnectionResetError, BrokenPipeError):
            pass
        except BaseException as error:
            if not self.stop.is_set():
                self.errors.append(error)

    def udp_loop(self, conn, udp, association):
        uplink = None
        peer = None
        seen = 0
        pending = None
        try:
            while not self.stop.is_set():
                readable, _, _ = select.select([conn, udp] + ([uplink] if uplink else []), [], [], 0.1)
                if conn in readable and not conn.recv(1):
                    return
                if udp in readable:
                    data, peer = udp.recvfrom(65536)
                    assert data[:3] == bytes(3)
                    packet = Packet(data[3:])
                    host, port = address(packet)
                    assert host in ("127.0.0.1", "::1", "only-in-tailnet.invalid")
                    if not uplink:
                        family = socket.AF_INET6 if host == "::1" else socket.AF_INET
                        uplink = socket.socket(family, socket.SOCK_DGRAM)
                        uplink.connect(("::1" if family == socket.AF_INET6 else "127.0.0.1", port))
                    seen += 1
                    if self.restart and association == 1 and seen == 9:
                        # Close both control and UDP sockets; the session must re-associate.
                        if self.udp_only:
                            # A broken UDP relay with a live TCP control channel
                            # must recover too (connected UDP reports ICMP refusal).
                            udp.close()
                            while conn.recv(1):
                                pass
                        return
                    if self.loss and seen % 7 == 0:
                        continue
                    # Send bogus datagrams from another UDP sender. The client's connected
                    # socket must ignore them before parsing/decryption.
                    if self.noisy and seen == 1:
                        with socket.socket(self.family, socket.SOCK_DGRAM) as intruder:
                            intruder.sendto(b"not a SOCKS packet", peer)
                    uplink.send(packet.data)
                    self.forwarded += 1
                    pending = data[:len(data) - len(packet.data)]
                if uplink and uplink in readable:
                    data = uplink.recv(65536)
                    if peer:
                        # Malformed FRAG, address type, truncation and wrong destination
                        # precede the valid reply. They must not disturb the session.
                        if self.noisy:
                            udp.sendto(b"\0\0\1" + pending[3:] + data, peer)
                            udp.sendto(b"\0\0\0\xff", peer)
                            udp.sendto(b"\0\0\0\4", peer)
                            wrong = pending[:-2] + struct.pack("!H", (port % 65535) + 1)
                            udp.sendto(wrong + data, peer)
                        udp.sendto(pending + data, peer)
        finally:
            if uplink:
                uplink.close()

    def close(self):
        self.stop.set()
        self.listener.close()
        self.worker.join(2)
        for worker in self.threads:
            worker.join(2)
        assert not self.errors, self.errors


@contextlib.contextmanager
def proxy(**kwargs):
    server = Proxy(**kwargs)
    try:
        yield server
    finally:
        server.close()


def test():
    driver = Path.cwd() / "socks5-proxy"
    wrapper = (Path.cwd() / "../../scripts/goblin-skiff").resolve()
    env = dict(os.environ, TERM="xterm-256color")
    for ipv6, target in ((False, "127.0.0.1"), (False, "only-in-tailnet.invalid"), (True, "::1")):
        with proxy(ipv6=ipv6, restart=True, wildcard=True) as server:
            result = subprocess.run([str(driver), server.endpoint, target], capture_output=True, text=True, timeout=15, env=env)
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert server.associations >= 2, "proxy restart was not exercised"
            print(result.stdout.strip())
    for options, message in ((dict(reject=True), "UDP ASSOCIATE rejected"), (dict(malformed=True), "malformed"), (dict(auth=True), "authentication")):
        with proxy(**options) as server:
            result = subprocess.run([str(driver), server.endpoint, "only-in-tailnet.invalid"], capture_output=True, text=True, timeout=5, env=env)
            assert result.returncode != 0 and message in result.stderr, result
            assert server.forwarded == 0
    with proxy(restart=True, udp_only=True) as server:
        result = subprocess.run([str(driver), server.endpoint, "only-in-tailnet.invalid"], capture_output=True, text=True, timeout=15, env=env)
        assert result.returncode == 0, (result.stdout, result.stderr)
        assert server.associations >= 2
        print("PASS: UDP-only relay loss recovers without waiting for TCP closure")
    for target in ("only-in-tailnet.invalid", "127.0.0.1", "::1"):
        with proxy() as server:
            # The IPv6 tailnet target must work through an IPv4-only local proxy.
            result = subprocess.run([str(wrapper), "--socks5-proxy=" + server.endpoint, "--family=inet6",
                                     "--fake-proxy", "--", target, "22"],
                                    input=b"login bytes\n", capture_output=True, timeout=5, env=env)
            assert result.returncode == 0, result.stderr
            assert result.stdout == b"SSH-through-SOCKS\nlogin bytes\n", result.stdout
            assert result.stderr == ("MOSH IP " + target + "\n").encode(), result.stderr
            assert server.requests == [(1, target, 22)], server.requests
    print("PASS: SOCKS5 CONNECT/UDP ASSOCIATE, proxy DNS, IPv4/IPv6, split handshakes, packet validation, loss, proxy restart and rejection")


if __name__ == "__main__":
    test()
