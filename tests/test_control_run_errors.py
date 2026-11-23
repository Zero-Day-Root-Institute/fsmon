#!/usr/bin/env python3
"""Check ctl run transport failures with a private local fake control peer."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading


def scenario(binary, directory, name, pause_reply, resume_reply, child_code, expected):
    endpoint = directory / (name + '.ctl')
    marker = directory / (name + '.executed')
    requests = []
    failures = []
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(str(endpoint))
    endpoint.chmod(0o600)
    listener.listen(2)
    listener.settimeout(12)

    def serve():
        try:
            for index, reply in enumerate((pause_reply, resume_reply)):
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(10)
                    request = bytearray()
                    while not request.endswith(b'\n'):
                        chunk = connection.recv(512)
                        assert chunk and len(request) < 512
                        request.extend(chunk)
                    requests.append(request.decode().strip().split())
                    if not index:
                        assert not marker.exists(), 'command started before pause ACK'
                    elif pause_reply is not None and pause_reply.get('ok'):
                        assert marker.exists(), 'resume ran before command completed'
                    if reply is not None:
                        connection.sendall((json.dumps(reply, separators=(',', ':')) + '\n').encode())
        except BaseException as error:
            failures.append(error)
        finally:
            listener.close()

    thread = threading.Thread(target=serve, daemon=True)
    thread.start()
    code = ("from pathlib import Path; import sys; "
            "Path(sys.argv[1]).write_text('executed'); "
            "print('child stdout'); print('child stderr', file=sys.stderr); "
            "raise SystemExit(int(sys.argv[2]))")
    result = subprocess.run([binary, 'ctl', 'run', '--socket', str(endpoint),
                             '--scope', 'report', '--ttl', '23', '--discard-pending',
                             '--', sys.executable, '-c', code, str(marker), str(child_code)],
                            capture_output=True, text=True, timeout=15)
    thread.join(timeout=13)
    assert not thread.is_alive(), 'control peer did not complete'
    assert not failures, failures
    assert len(requests) == 2, requests
    pause, resume = requests
    assert len(pause) == 5 and pause[0] == 'PAUSE' and pause[2:] == ['report', '23', '1'], requests
    assert pause[1].startswith('run-') and len(pause[1]) < 128, requests
    assert resume == ['RESUME', pause[1]], 'did not release exactly its own generated ID'
    assert result.returncode == expected, (name, result)
    executed = bool(pause_reply and pause_reply.get('ok'))
    assert marker.exists() == executed, (name, result)
    assert result.stdout == ('child stdout\n' if executed else ''), (name, result)
    if executed:
        assert 'child stderr' in result.stderr, (name, result)
    if not pause_reply or not pause_reply.get('ok'):
        assert 'pause failed' in result.stderr or 'invalid response' in result.stderr, result
    if not resume_reply or not resume_reply.get('ok'):
        assert 'resume failed' in result.stderr and 'TTL' in result.stderr, result
    return pause[1]


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: test_control_run_errors.py FSMOD_BINARY')
    binary = str(Path(sys.argv[1]).resolve())
    ok = {'ok': True, 'pid': os.getpid()}
    error = {'ok': False, 'error': 'simulated control rejection'}
    with tempfile.TemporaryDirectory(prefix='fmrun-', dir='/tmp') as temporary:
        base = Path(temporary)
        owners = [
            scenario(binary, base, 'rejected', error, ok, 0, 1),
            scenario(binary, base, 'lost-ack', None, ok, 0, 1),
            scenario(binary, base, 'cleanup-failed', ok, error, 0, 1),
            scenario(binary, base, 'child-failed', ok, error, 7, 7),
            scenario(binary, base, 'cleanup-lost', ok, None, 0, 1),
            scenario(binary, base, 'success', ok, ok, 0, 0),
        ]
        assert len(owners) == len(set(owners)), 'run owner IDs were reused'
    print('control run failure-path tests passed')


if __name__ == '__main__':
    main()
