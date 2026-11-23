#!/usr/bin/env python3
"""Local-only end-to-end daemon checks. No external reporting endpoint is used."""
import http.server
import json
import os
import platform
from pathlib import Path
import queue
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
import uuid


def wait_for(predicate, seconds=12):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError("condition did not become true before timeout")


HOST_METADATA_FIELDS = ("uuid", "os_name", "os_version", "kernel_version")


def decode_envelope(payload):
    assert isinstance(payload, dict), payload
    assert set(payload) == {"source", "data"}, payload
    assert payload["source"] == "fsmod", payload
    assert isinstance(payload["data"], dict), payload
    return payload["data"]


def test_envelope_decoder():
    event = {"filename": "/one", "uid": 424242, "uuid": None}
    assert decode_envelope(json.loads(json.dumps({"source": "fsmod", "data": event}))) == event
    invalid = [
        event, [], {"source": "wrong", "data": event},
        {"source": "fsmod", "data": []},
        {"source": "fsmod", "data": None},
        {"source": "fsmod", "data": event, "filename": "/outside"},
        {"source": "fsmod"},
    ]
    for payload in invalid:
        try:
            decode_envelope(payload)
        except AssertionError:
            pass
        else:
            raise AssertionError(("invalid report envelope accepted", payload))


def assert_host_metadata(event, backend, expected=None):
    assert isinstance(event, dict), event
    assert event.get("backend") == backend, event
    for key in HOST_METADATA_FIELDS:
        assert key in event, (key, event)
        value = event[key]
        assert value is None or (isinstance(value, str) and value), (key, value)
    assert isinstance(event["os_name"], str) and event["os_name"], event
    assert event["kernel_version"] == platform.release(), event
    identifier = event["uuid"]
    if identifier is not None:
        if sys.platform == "darwin":
            assert re.fullmatch(r"[0-9A-F]{8}(?:-[0-9A-F]{4}){3}-[0-9A-F]{12}", identifier), identifier
            assert uuid.UUID(identifier).int != 0, identifier
        else:
            assert re.fullmatch(r"[0-9a-f]{32}", identifier), identifier
            assert int(identifier, 16) != 0, identifier
    if sys.platform == "darwin":
        assert event["os_name"] == "macOS", event
        product_version = platform.mac_ver()[0]
        if product_version:
            assert event["os_version"] == product_version, event
    elif sys.platform.startswith("linux"):
        # machine-id and os-release may legitimately be absent in containers.
        # When the normal host sources are available, verify their actual data.
        for path in ("/etc/machine-id", "/var/lib/dbus/machine-id"):
            try:
                candidate = Path(path).read_text(encoding="ascii").rstrip("\r\n")
            except (OSError, UnicodeError):
                continue
            if re.fullmatch(r"[0-9a-f]{32}", candidate) and int(candidate, 16):
                assert identifier == candidate, event
                break
        try:
            release = platform.freedesktop_os_release()
        except (AttributeError, OSError):
            release = {}
        if release.get("NAME"):
            assert event["os_name"] == release["NAME"], event
        if release.get("VERSION_ID"):
            assert event["os_version"] == release["VERSION_ID"], event
    # The host identifier is separate from the numeric originating user/group.
    assert type(event.get("uid")) is int and type(event.get("gid")) is int, event
    if backend == "inotify":
        assert event["uid"] == 0 and event["gid"] == 0, event
    current = {key: event[key] for key in HOST_METADATA_FIELDS}
    if expected is not None:
        assert current == expected, (current, expected)
    return current


def main():
    test_envelope_decoder()
    binary = str(Path(sys.argv[1]).resolve())
    # Keep the loopback integration server independent of machine proxy setup.
    os.environ["NO_PROXY"] = "127.0.0.1,localhost"
    os.environ["no_proxy"] = "127.0.0.1,localhost"
    received = queue.Queue()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            body = self.rfile.read(int(self.headers["Content-Length"]))
            received.put((decode_envelope(json.loads(body)), self.headers.get("Authorization")))
            response = json.dumps({"code": 200, "message": "ok"}).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)

        def log_message(self, *_):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="fsmod-integration-") as directory:
            base = Path(directory).resolve()
            first, second = base / "first", base / "second"
            first.mkdir()
            second.mkdir()
            excluded = first / "excluded"
            excluded.mkdir()
            single = base / "single-file"
            single.write_text("initial", encoding="utf-8")
            pidfile = base / "fsmod.pid"
            config = base / "fsmod.conf"
            log = base / "daemon.log"
            backend = "fsevapi" if sys.platform == "darwin" else "inotify"
            config.write_text(
                f"report_url=http://127.0.0.1:{server.server_port}/events\n"
                "report_token=integration-token\n"
                f"backend={backend}\nwatch_path={first}\nwatch_path={second}\n"
                f"watch_path={single}\nexclude_path={excluded}\n"
                f"pid_file={pidfile}\nlog_file={log}\n"
                "daemonize=true\nreport_retries=1\nretry_delay_seconds=0\n"
                "report_timeout_seconds=2\nshutdown_timeout_seconds=5\n",
                encoding="utf-8",
            )
            cmd = [binary, "-C", str(config)]
            subprocess.run(cmd + ["-T"], check=True, timeout=10)
            metadata = None

            def collect_expected(expected, rename_target=None):
                nonlocal metadata
                found = set()
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline and found != expected:
                    try:
                        event, auth = received.get(timeout=0.2)
                    except queue.Empty:
                        continue
                    assert auth == "Bearer integration-token", auth
                    metadata = assert_host_metadata(event, backend, metadata)
                    path = event.get("filename", "")
                    assert not path.startswith(str(excluded) + "/"), event
                    target = event.get("newfile") or ""
                    assert not target.startswith(str(excluded) + "/"), event
                    if rename_target is not None:
                        if path not in expected or event.get("type") != "FSE_RENAME" or target != rename_target:
                            continue
                    found.update(expected.intersection({path, target}))
                assert found == expected, (found, expected)

            def assert_no_read_feedback(paths):
                # Flush prior changes, then only read files. The daemon must
                # not enqueue access/open/close-nowrite events on Linux.
                time.sleep(0.6)
                while not received.empty():
                    received.get_nowait()
                for path in paths:
                    path.read_bytes()
                deadline = time.monotonic() + 0.6
                while time.monotonic() < deadline:
                    try:
                        event, _ = received.get(timeout=0.1)
                    except queue.Empty:
                        continue
                    assert event.get("filename") not in {str(path) for path in paths}, event

            # Foreground mode exercises the same PID lock and graceful signal path.
            with log.open("ab") as output:
                process = subprocess.Popen(cmd + ["-F"], stdout=output, stderr=output)
                try:
                    wait_for(pidfile.exists)
                    time.sleep(0.8)  # allow backend and report worker initialization
                    assert process.poll() is None
                    duplicate = subprocess.run(cmd + ["-F"], capture_output=True, timeout=10)
                    assert duplicate.returncode != 0, "duplicate instance accepted"
                    a = first / '中文-"quote.txt'
                    b = second / "second.txt"
                    a.write_text("a", encoding="utf-8")
                    b.write_text("b", encoding="utf-8")
                    single.write_text("updated", encoding="utf-8")
                    (excluded / "hidden.txt").write_text("excluded", encoding="utf-8")
                    collect_expected({str(a), str(b), str(single)})
                    # Replacement keeps a single-file root covered by its parent.
                    time.sleep(0.6)
                    while not received.empty():
                        received.get_nowait()
                    replacement = base / "single-file.replacement"
                    replacement.write_text("replacement", encoding="utf-8")
                    os.replace(replacement, single)
                    collect_expected({str(single)})
                    nested = first / "new-directory"
                    nested.mkdir()
                    time.sleep(0.5)  # inotify must install the new subtree watch
                    nested_file = nested / "nested.txt"
                    nested_file.write_text("nested", encoding="utf-8")
                    collect_expected({str(nested_file)})
                    renamed = second / "renamed.txt"
                    os.rename(b, renamed)
                    if sys.platform == "darwin":
                        # FSEvents coalesces rename sides and does not pair them.
                        collect_expected({str(renamed)})
                    else:
                        collect_expected({str(b)}, rename_target=str(renamed))
                        assert_no_read_feedback([a, single, renamed])
                    subprocess.run(cmd + ["-S"], check=True, timeout=10)
                    assert process.wait(timeout=10) == 0
                    assert not pidfile.exists(), "PID file left behind"
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=10)

            # Double-fork mode returns only after both backend and reporter start.
            subprocess.run(cmd, check=True, timeout=10)
            daemon_pid = int(pidfile.read_text().strip())
            try:
                assert daemon_pid != os.getpid()
                c = second / "background.txt"
                c.write_text("background", encoding="utf-8")
                collect_expected({str(c)})
                # Stop must still work if a configured file root disappeared.
                single.unlink()
                subprocess.run(cmd + ["-S"], check=True, timeout=10)
                wait_for(lambda: not pidfile.exists())
            finally:
                if pidfile.exists():
                    try:
                        os.kill(daemon_pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
            assert subprocess.run(cmd + ["-S"], capture_output=True, timeout=10).returncode != 0
            print("daemon integration tests passed")
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
