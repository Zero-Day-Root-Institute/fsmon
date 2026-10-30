#!/usr/bin/env python3
"""Exercise default-config startup against a loopback-only HTTP endpoint.

Compile the supplied fixture binary with FSMOD_DEFAULT_CONFIG set to argv[2].
The fixture path must not already exist; this runner owns it only for the test.
"""
import http.server
import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import sys
import tempfile
import threading
import time

from test_daemon import decode_envelope, wait_for


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_default_startup.py FIXTURE_BINARY DEFAULT_CONFIG_PATH")
    binary = str(Path(sys.argv[1]).resolve())
    default = Path(sys.argv[2]).resolve()
    assert not default.exists(), ("refusing to overwrite existing config", default)
    os.environ["NO_PROXY"] = os.environ["no_proxy"] = "127.0.0.1,localhost"
    received = queue.Queue()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            received.put((decode_envelope(payload), self.headers.get("Authorization")))
            body = b'{"code":200,"message":"ok"}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    backend = "fsevapi" if sys.platform == "darwin" else "inotify"
    owns_default = False

    def run(*args, success=True):
        result = subprocess.run([binary, *map(str, args)], capture_output=True,
                                text=True, timeout=10)
        assert (result.returncode == 0) == success, (args, result)
        return result

    def expect_report(path, token):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            try:
                event, auth = received.get(timeout=0.2)
            except queue.Empty:
                continue
            if str(path) in (event.get("filename"), event.get("newfile")):
                assert auth == "Bearer " + token, (auth, event)
                assert event.get("backend") == backend, event
                return
        raise AssertionError(("configured event not reported", str(path)))

    try:
        with tempfile.TemporaryDirectory(prefix="fsmod-default-startup-") as directory:
            base = Path(directory).resolve()
            watched, alternate = base / "watched", base / "alternate"
            watched.mkdir()
            alternate.mkdir()
            pidfile, alternate_pid = base / "default.pid", base / "alternate.pid"
            output = base / "output.log"
            explicit = base / "explicit.conf"

            def config_text(root, pid, token, daemonize):
                return (f"report_url=http://127.0.0.1:{server.server_port}/events\n"
                        f"report_token={token}\nbackend={backend}\nwatch_path={root}\n"
                        f"pid_file={pid}\nlog_file={base / 'daemon.log'}\n"
                        f"daemonize={str(daemonize).lower()}\n"
                        "report_retries=0\nreport_timeout_seconds=2\nshutdown_timeout_seconds=3\n")

            # A missing default must fail, never silently monitor the whole system.
            for args in ((), ("-T",), ("-F",), ("-S",), ("-D",)):
                result = run(*args, success=False)
                assert "Configuration:" in result.stderr, result
            with default.open("x", encoding="utf-8") as stream:
                owns_default = True
                # Implicit startup must stay foreground even with this setting.
                stream.write(config_text(watched, pidfile, "default-token", True))
            explicit.write_text(config_text(alternate, alternate_pid, "explicit-token", False),
                                encoding="utf-8")

            checked = run("-T")
            assert "Configuration OK:" in checked.stdout, checked
            assert "daemonize=false" in checked.stdout, checked
            assert not pidfile.exists()

            def foreground(args, pid, root, token, marker, stop_args):
                with output.open("ab") as log:
                    process = subprocess.Popen([binary, *map(str, args)], stdout=log, stderr=log)
                    try:
                        wait_for(lambda: pid.exists() and pid.read_text().strip().isdigit())
                        assert process.poll() is None, output.read_text()
                        assert int(pid.read_text()) == process.pid, "startup detached unexpectedly"
                        time.sleep(0.6)
                        path = root / marker
                        path.write_text(marker, encoding="utf-8")
                        expect_report(path, token)
                        run(*stop_args)
                        assert process.wait(timeout=8) == 0, output.read_text()
                        assert not pid.exists()
                    finally:
                        if pid.exists():
                            # Also clean up a mistakenly detached child on failure.
                            run(*stop_args)
                            wait_for(lambda: not pid.exists())
                        if process.poll() is None:
                            process.terminate()
                            process.wait(timeout=8)

            foreground([], pidfile, watched, "default-token", "noargs.txt", ["-S"])
            foreground(["-F"], pidfile, watched, "default-token", "foreground.txt", ["-S"])

            # Explicit -D remains the opt-in detach path, using the same default.
            run("-D")
            daemon_pid = int(pidfile.read_text())
            try:
                path = watched / "background.txt"
                path.write_text("background", encoding="utf-8")
                expect_report(path, "default-token")
                run("-S")
                wait_for(lambda: not pidfile.exists())
            finally:
                if pidfile.exists():
                    try:
                        os.kill(daemon_pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass

            # An explicit file selects its own root, PID file, and auth token.
            run("-C", explicit, "-T")
            foreground(["-C", explicit], alternate_pid, alternate, "explicit-token",
                       "override.txt", ["-C", explicit, "-S"])
            assert not pidfile.exists()
            for args in (("-D", "-F"), ("-S", "-T"), ("-S", "-F"),
                         ("-F", "-J"), ("-T", watched), ("-C", explicit, "-J")):
                run(*args, success=False)
            assert not pidfile.exists() and not alternate_pid.exists()

            # Missing watch roots warn without disabling existing roots.
            missing_file = base / "missing-file"
            missing_nested = base / "missing-parent" / "nested"
            default.write_text(
                config_text(watched, pidfile, "mixed-token", False) +
                f"watch_path={missing_file}\nwatch_path={missing_nested}\n",
                encoding="utf-8")
            checked = run("-T")
            assert "watch_paths=1" in checked.stdout and "warnings=2" in checked.stdout, checked
            for path in (missing_file, missing_nested):
                assert str(path) in checked.stderr and "ignored until restart" in checked.stderr, checked
            foreground([], pidfile, watched, "mixed-token", "mixed-roots.txt", ["-S"])
            assert str(missing_file) in output.read_text(), output.read_text()

            # Per-root event rules keep create/modify but suppress deletion in
            # one root, while a second root retains all backend events.
            default.write_text(
                config_text(watched, pidfile, "events-token", False) +
                f"watch_events=create,modify\nwatch_path={alternate}\nwatch_events=all\n",
                encoding="utf-8")
            run("-T")
            event_output = base / "events.log"
            with event_output.open("wb") as log:
                process = subprocess.Popen([binary], stdout=log, stderr=log)
                try:
                    wait_for(lambda: "fsmod started:" in event_output.read_text())
                    limited_file = watched / "limited-events.txt"
                    all_file = alternate / "all-events.txt"
                    limited_file.write_text("create", encoding="utf-8")
                    expect_report(limited_file, "events-token")
                    all_file.write_text("create", encoding="utf-8")
                    expect_report(all_file, "events-token")
                    time.sleep(0.7)
                    while not received.empty():
                        received.get_nowait()
                    limited_file.write_text("modify", encoding="utf-8")
                    expect_report(limited_file, "events-token")
                    time.sleep(0.7)
                    while not received.empty():
                        received.get_nowait()
                    limited_file.unlink()
                    all_file.unlink()
                    saw_all_delete = False
                    deadline = time.monotonic() + 3
                    while time.monotonic() < deadline:
                        try:
                            event, auth = received.get(timeout=0.2)
                        except queue.Empty:
                            continue
                        if event.get("type") == "FSE_DELETE":
                            assert event.get("filename") != str(limited_file), event
                            if event.get("filename") == str(all_file):
                                saw_all_delete = True
                                assert auth == "Bearer events-token", auth
                    assert saw_all_delete, event_output.read_text()
                    run("-S")
                    assert process.wait(timeout=8) == 0, event_output.read_text()
                    assert not pidfile.exists()
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=8)

            # All missing roots must stay idle, never use / or . as a fallback.
            idle_root = base / "missing-idle"
            default.write_text(config_text(idle_root, pidfile, "idle-token", False),
                               encoding="utf-8")
            checked = run("-T")
            assert "watch_paths=0" in checked.stdout and "warnings=1" in checked.stdout, checked
            assert "monitoring is idle until restart" in checked.stderr, checked
            time.sleep(0.5)
            while not received.empty():
                received.get_nowait()
            idle_output = base / "idle.log"
            with idle_output.open("wb") as log:
                process = subprocess.Popen([binary], stdout=log, stderr=log)
                try:
                    wait_for(lambda: "roots=0 (idle)" in idle_output.read_text())
                    assert process.poll() is None, idle_output.read_text()
                    (base / "outside-idle.txt").write_text("not watched", encoding="utf-8")
                    idle_root.mkdir()
                    (idle_root / "created-after-start.txt").write_text("still skipped", encoding="utf-8")
                    time.sleep(0.6)
                    assert received.empty(), "idle startup unexpectedly reported an event"
                    run("-S")
                    assert process.wait(timeout=8) == 0, idle_output.read_text()
                    assert not pidfile.exists()
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=8)
            assert str(idle_root) in idle_output.read_text(), idle_output.read_text()

            # Background warnings belong in the redirected daemon log too.
            absent_background = base / "missing-background"
            default.write_text(config_text(absent_background, pidfile, "idle-token", True),
                               encoding="utf-8")
            run("-D")
            try:
                daemon_log = base / "daemon.log"
                wait_for(lambda: str(absent_background) in daemon_log.read_text())
                assert "roots=0 (idle)" in daemon_log.read_text(), daemon_log.read_text()
                run("-S")
                wait_for(lambda: not pidfile.exists())
            finally:
                if pidfile.exists():
                    os.kill(int(pidfile.read_text()), signal.SIGTERM)
                    wait_for(lambda: not pidfile.exists())

            # Legacy path/JSON/alarm mode must work even with no default config.
            default.unlink()
            owns_default = False
            legacy_output = base / "legacy.log"
            with legacy_output.open("wb") as log:
                process = subprocess.Popen([binary, "-B", backend, "-J", "-a", "3",
                                            str(watched)], stdout=log, stderr=log)
                try:
                    time.sleep(0.7)
                    assert process.poll() is None, legacy_output.read_text()
                    marker = watched / "legacy.txt"
                    marker.write_text("legacy", encoding="utf-8")
                    assert process.wait(timeout=8) == 0, legacy_output.read_text()
                finally:
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=8)
            events = [json.loads(line) for line in legacy_output.read_text().splitlines()
                      if line.startswith("{")]
            assert any(event.get("filename") == str(marker) for event in events), events
            assert not pidfile.exists()
            print("default-config startup integration tests passed")
    finally:
        if owns_default:
            default.unlink()
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
