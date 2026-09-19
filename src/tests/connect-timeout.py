# Modified for Goblin Skiff on 2026-09-19.
"""Exercise wrapper forwarding and actual client startup deadlines on loopback."""
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import socket
import struct
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[2]
CLIENT = os.environ.get('GOBLIN_SKIFF_TEST_CLIENT', str(ROOT / 'src/frontend/goblin-skiff-client'))
WRAPPER = os.environ.get('GOBLIN_SKIFF_TEST_WRAPPER', str(ROOT / 'scripts/goblin-skiff'))

for bad in ('0', '-1', '3601', '1.5', 'bad'):
    for program in (CLIENT, WRAPPER):
        result = subprocess.run([program, '--connect-timeout=' + bad, '127.0.0.1', '60000'], capture_output=True, text=True, timeout=3)
        assert result.returncode and 'connect-timeout' in result.stderr, (program, bad, result.stderr)

with tempfile.TemporaryDirectory() as folder:
    folder = Path(folder)
    fake_client, fake_ssh = folder / 'client', folder / 'ssh'
    fake_client.write_text('''#!/usr/bin/env python3
import json,os,sys
if '-c' in sys.argv: print(256)
elif '--image-codecs' in sys.argv: print('webp')
elif '--connection-version' in sys.argv: sys.exit(1)
else: open(os.environ['TIMEOUT_CLIENT_ARGS'],'w').write(json.dumps(sys.argv))
''')
    fake_ssh.write_text('''#!/usr/bin/env python3
import json,os,sys
if '-G' in sys.argv: print('hostname 127.0.0.1')
else:
 open(os.environ['TIMEOUT_SSH_ARGS'],'w').write(json.dumps(sys.argv))
 print('MOSH IP 127.0.0.1')
 print('MOSH CONNECT 60000 AAAAAAAAAAAAAAAAAAAAAA')
''')
    fake_client.chmod(0o755); fake_ssh.chmod(0o755)
    env = dict(os.environ, TIMEOUT_CLIENT_ARGS=str(folder / 'client.json'), TIMEOUT_SSH_ARGS=str(folder / 'ssh.json'))
    for value in (None, '7'):
        for proxy in (False, True):
            args = [WRAPPER, '--client=' + str(fake_client), '--ssh=' + str(fake_ssh)]
            if value: args += ['--connect-timeout=' + value]
            if proxy: args += ['--socks5-proxy=127.0.0.1:1080']
            result = subprocess.run(args + ['test-host'], env=env, text=True, capture_output=True, timeout=5)
            assert result.returncode == 0, result.stderr
            expected = '--connect-timeout=' + (value or '120')
            assert expected in json.loads((folder / 'client.json').read_text())
            assert any(expected in arg for arg in json.loads((folder / 'ssh.json').read_text()))


def blackhole(timeout=None):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.bind(('127.0.0.1', 0))
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
        args = [CLIENT, '--no-kitty', '--no-sixel', '--mascot=none']
        if timeout is not None: args += ['--connect-timeout=' + str(timeout)]
        env = dict(os.environ, TERM='xterm-256color', MOSH_KEY='AAAAAAAAAAAAAAAAAAAAAA', GOBLIN_SKIFF_COMPACT_KEEPALIVE='0', GOBLIN_SKIFF_LINK_BUDGET='0')
        proc = subprocess.Popen(args + ['127.0.0.1', str(udp.getsockname()[1])], stdin=slave, stdout=slave, stderr=slave, env=env, start_new_session=True)
        os.close(slave)
        output = bytearray(); start = time.monotonic()
        limit = 10 if timeout is not None else 21
        try:
            while time.monotonic() - start < limit and proc.poll() is None:
                if select.select([master], [], [], 0.1)[0]:
                    try: output.extend(os.read(master, 65536))
                    except OSError: break
            if timeout is None:
                assert proc.poll() is None, 'default still expires after 15 seconds: ' + repr(output[-500:])
            else:
                proc.wait(timeout=2)
                assert proc.returncode != 0 and b'did not make a successful connection' in output, repr(output[-500:])
                assert timeout <= time.monotonic() - start < 10
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL); proc.wait()
            os.close(master)

blackhole(2)
blackhole()
print('PASS: CLI validation, wrapper/client/proxy forwarding, configurable deadline, longer default')
