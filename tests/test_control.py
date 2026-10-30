#!/usr/bin/env python3
"""Exercise runtime control against a loopback HTTP server and real backend.

Usage: python3 tests/test_control.py ./fsmod
Runs without root on macOS (fsevapi) and Linux (inotify). No external endpoint
is contacted. Temporary configuration, watch roots and leases belong to this
test only. Reporter transport cancellation has additional C unit coverage.
"""
import http.server
import json
import os
from pathlib import Path
import socket
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time


def wait_for(predicate, seconds=12, message="condition timed out"):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError(message)


class Reports:
    def __init__(self):
        self.lock = threading.Lock()
        self.events = []

    def append(self, payload):
        assert payload.get("source") == "fsmod", payload
        assert isinstance(payload.get("data"), dict), payload
        with self.lock:
            self.events.append(payload["data"])

    def has(self, path):
        path = str(path)
        with self.lock:
            return any(path in (event.get("filename"), event.get("newfile"))
                       for event in self.events)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_control.py FSMOD_BINARY")
    if sys.platform != "darwin" and not sys.platform.startswith("linux"):
        raise SystemExit("runtime control integration requires macOS or Linux")
    binary = str(Path(sys.argv[1]).resolve())
    env = dict(os.environ, NO_PROXY="127.0.0.1,localhost",
               no_proxy="127.0.0.1,localhost")
    received = Reports()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            payload = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            received.append(payload)
            body = b'{"code":200,"message":"ok"}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def log_message(self, *_):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        # Unix-domain sockaddr paths are short on Darwin. /tmp is used on purpose.
        with tempfile.TemporaryDirectory(prefix="fmctl-", dir="/tmp") as directory:
            base = Path(directory).resolve()
            watched = base / "watch"
            watched.mkdir()
            config = base / "fsmod.conf"
            pidfile = base / "fsmod.pid"
            socket_path = Path(str(pidfile) + ".ctl")
            log = base / "events.log"
            backend = "fsevapi" if sys.platform == "darwin" else "inotify"
            config.write_text(
                f"report_url=http://127.0.0.1:{server.server_port}/events\n"
                f"backend={backend}\nwatch_path={watched}\n"
                f"pid_file={pidfile}\nlog_file={log}\n"
                "daemonize=false\nlog_events=true\nqueue_capacity=64\n"
                "report_timeout_seconds=2\nreport_retries=0\n"
                "retry_delay_seconds=0\nshutdown_timeout_seconds=3\n",
                encoding="utf-8",
            )
            config.chmod(0o600)
            output = log.open("ab")
            process = None
            sequence = 0
            active_wrappers = []

            def ctl(*args, success=True, timeout=12):
                result = subprocess.run(
                    [binary, "ctl", *map(str, args), "--socket", str(socket_path)],
                    env=env, capture_output=True, text=True, timeout=timeout,
                )
                assert (result.returncode == 0) == success, (args, result)
                return result

            def status():
                result = ctl("status", "--json")
                state = json.loads(result.stdout)
                assert isinstance(state, dict) and state.get("ok") is True, state
                assert state["pid"] == process.pid, state
                for key in ("monitor_paused", "report_paused", "in_flight"):
                    assert type(state[key]) is bool, (key, state)
                for key in ("queued", "delivered", "failed", "dropped",
                            "suppressed_monitor", "suppressed_report"):
                    assert type(state[key]) is int and state[key] >= 0, (key, state)
                assert isinstance(state["holds"], list), state
                owners = [hold["id"] for hold in state["holds"]]
                assert len(owners) == len(set(owners)), state
                for hold in state["holds"]:
                    assert hold["scope"] in ("all", "report"), state
                    assert type(hold["remaining_seconds"]) in (int, float), state
                    assert 0 <= hold["remaining_seconds"] <= 86400, state
                return state

            def has_owner(owner):
                return any(hold["id"] == owner for hold in status()["holds"])

            def pause(owner, scope="all", ttl=30, discard=False):
                args = ["pause", "--id", owner, "--scope", scope, "--ttl", ttl]
                if discard:
                    args.append("--discard-pending")
                ctl(*args)
                assert has_owner(owner), status()
                state = status()
                assert state["report_paused"] and not state["in_flight"], state
                assert any(hold["id"] == owner and hold["scope"] == scope
                           for hold in state["holds"]), state
                state_path = Path(str(pidfile) + ".ctl.state")
                assert stat.S_IMODE(state_path.stat().st_mode) == 0o600

            def resume(owner):
                ctl("resume", "--id", owner)
                assert not has_owner(owner), status()

            def log_has(path):
                for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
                    try:
                        event = json.loads(line).get("data", {})
                    except (ValueError, AttributeError):
                        continue
                    if str(path) in (event.get("filename"), event.get("newfile")):
                        return True
                return False

            def create(label):
                nonlocal sequence
                sequence += 1
                path = watched / ("%02d-%s.txt" % (sequence, label))
                path.write_text(label, encoding="utf-8")
                return path

            def run_args(command, scope="all", ttl=30, config_lookup=False, discard=False):
                args = [binary]
                if config_lookup:
                    args += ["-C", str(config)]
                args += ["ctl", "run", "--scope", scope, "--ttl", str(ttl)]
                if discard:
                    args.append("--discard-pending")
                if not config_lookup:
                    args += ["--socket", str(socket_path)]
                return args + ["--", *map(str, command)]

            def assert_no_run_hold():
                assert not any(hold["id"].startswith("run-")
                               for hold in status()["holds"]), status()

            def run_live(label, scope="all", ttl=30, exit_code=0,
                         config_lookup=False, extra_args=(), discard=False):
                nonlocal sequence
                sequence += 1
                event = watched / ("%02d-%s.txt" % (sequence, label))
                ready = base / (label + ".ready")
                release = base / (label + ".release")
                child_code = (
                    "import json,os,subprocess,sys,time; from pathlib import Path; "
                    "state=json.loads(subprocess.check_output([sys.argv[1],'ctl','status',"
                    "'--json','--socket',sys.argv[2]],text=True)); "
                    "Path(sys.argv[3]).write_text(json.dumps({'status':state,'pid':os.getpid(),"
                    "'args':sys.argv[7:]})); Path(sys.argv[5]).write_text('child mutation'); "
                    "print('child stdout',flush=True); print('child stderr',file=sys.stderr,flush=True); "
                    "deadline=time.monotonic()+20\n"
                    "while not Path(sys.argv[4]).exists() and time.monotonic()<deadline: time.sleep(.02)\n"
                    "sys.exit(int(sys.argv[6]) if Path(sys.argv[4]).exists() else 99)\n"
                )
                old_owners = {hold["id"] for hold in status()["holds"]}
                child = [sys.executable, "-c", child_code, binary, socket_path,
                         ready, release, event, exit_code, *extra_args]
                wrapper = subprocess.Popen(run_args(child, scope, ttl, config_lookup, discard),
                                           env=env, stdout=subprocess.PIPE,
                                           stderr=subprocess.PIPE, text=True,
                                           start_new_session=True)
                active_wrappers.append(wrapper)
                wait_for(lambda: ready.exists() or wrapper.poll() is not None,
                         message="ctl run child did not execute: " + label)
                assert ready.exists(), (label, wrapper.communicate(timeout=2))
                observed = json.loads(ready.read_text())
                holds = observed["status"]["holds"]
                own = [hold for hold in holds if hold["id"] not in old_owners]
                assert len(own) == 1 and own[0]["scope"] == scope, observed
                assert observed["status"]["report_paused"], observed
                assert observed["status"]["monitor_paused"] == (scope == "all" or
                    any(hold["scope"] == "all" for hold in holds)), observed
                assert not observed["status"]["in_flight"], observed
                assert observed["args"] == list(extra_args), observed
                assert wrapper.poll() is None, label
                return wrapper, release, event, own[0]["id"]

            def finish_run(wrapper, release, owner, expected=0):
                release.touch()
                stdout, stderr = wrapper.communicate(timeout=12)
                assert wrapper.returncode == expected, (wrapper.returncode, stdout, stderr)
                assert stdout == "child stdout\n", stdout
                assert "child stderr\n" in stderr, stderr
                assert not has_owner(owner), status()

            def test_run_commands():
                # Omitting scope/TTL selects all/600 while keeping control
                # diagnostics out of the child's JSON standard output.
                command = [binary, "ctl", "status", "--json", "--socket", str(socket_path)]
                defaults = subprocess.run([binary, "ctl", "run", "--socket", str(socket_path),
                                           "--", *command], env=env, capture_output=True,
                                          text=True, timeout=12)
                assert defaults.returncode == 0, defaults
                default_state = json.loads(defaults.stdout)
                assert default_state["monitor_paused"] and default_state["report_paused"], default_state
                assert len(default_state["holds"]) == 1, default_state
                default_hold = default_state["holds"][0]
                assert default_hold["id"].startswith("run-") and default_hold["scope"] == "all", default_hold
                assert 590 <= default_hold["remaining_seconds"] <= 600, default_hold
                assert_no_run_hold()

                wrapper, release, event, owner = run_live("run-all", discard=True)
                expect(event, http=False, logged=False)
                finish_run(wrapper, release, owner)
                expect(create("after-run-all"))
                assert not received.has(event) and not log_has(event), "run all event replayed"

                # Child --socket and option-looking arguments must be passed
                # literally; only options before -- affect config discovery.
                wrapper, release, event, owner = run_live(
                    "run-report", scope="report", config_lookup=True,
                    extra_args=("--socket", "/not-the-control-socket", "--ttl", "0", "$(touch nope)"))
                expect(event, http=False, logged=True)
                finish_run(wrapper, release, owner)
                expect(create("after-run-report"))
                assert not received.has(event), "run report event replayed"

                pause("com.example.external-owner", scope="report")
                wrapper, release, event, owner = run_live("run-other-owner")
                expect(event, http=False, logged=False)
                finish_run(wrapper, release, owner)
                assert has_owner("com.example.external-owner"), status()
                assert status()["report_paused"] and not status()["monitor_paused"], status()
                resume("com.example.external-owner")

                # Nonzero child status must survive successful hold cleanup.
                wrapper, release, event, owner = run_live("run-exit-seven", exit_code=7)
                expect(event, http=False, logged=False)
                finish_run(wrapper, release, owner, expected=7)

                for command, code in (([str(base / "missing-command")], 127),
                                      ([str(base / "not-executable")], 126)):
                    (base / "not-executable").write_text("this file is not executable\n")
                    (base / "not-executable").chmod(0o600)
                    result = subprocess.run(run_args(command), env=env, capture_output=True,
                                            text=True, timeout=12)
                    assert result.returncode == code, result
                    assert result.stdout == "", result
                    assert result.stderr, result
                    assert_no_run_hold()

                marker = base / "must-not-execute"
                touch_command = [sys.executable, "-c",
                                 "from pathlib import Path; import sys; Path(sys.argv[1]).touch()", marker]
                prefix = [binary, "ctl", "run", "--socket", str(socket_path)]
                for suffix in ([*map(str, touch_command)], ["--"],
                               ["--id", "caller-owner", "--", *map(str, touch_command)],
                               ["--json", "--", *map(str, touch_command)],
                               ["--scope", "bad", "--", *map(str, touch_command)],
                               ["--ttl", "0", "--", *map(str, touch_command)],
                               ["--ttl", "86401", "--", *map(str, touch_command)]):
                    result = subprocess.run(prefix + suffix, env=env, capture_output=True,
                                            text=True, timeout=12)
                    assert result.returncode != 0, result
                    assert not marker.exists(), "invalid ctl run executed command"
                    assert_no_run_hold()

                # Run a separate, single-threaded Python pty harness so the
                # child has a genuine controlling terminal. A missing child
                # foreground-group handoff leaves readline stopped by SIGTTIN.
                terminal_child = [sys.executable, "-u", "-c",
                                  "print('terminal-ready',flush=True); "
                                  "print('terminal-value='+input(),flush=True)"]
                terminal_harness = (
                    "import errno,json,os,pty,select,signal,sys,time\n"
                    "pid,fd=pty.fork()\n"
                    "if pid==0: os.execvpe(sys.argv[1],sys.argv[1:],os.environ)\n"
                    "data=bytearray(); sent=False; done=False; result=None\n"
                    "try:\n"
                    " deadline=time.monotonic()+8\n"
                    " while time.monotonic()<deadline:\n"
                    "  if select.select([fd],[],[],.05)[0]:\n"
                    "   try: part=os.read(fd,4096)\n"
                    "   except OSError as e:\n"
                    "    if e.errno!=errno.EIO: raise\n"
                    "    part=b''\n"
                    "   data.extend(part)\n"
                    "  if not sent and b'terminal-ready' in data:\n"
                    "   os.write(fd,b'fsmod-tty-input\\n'); sent=True\n"
                    "  exited,status=os.waitpid(pid,os.WNOHANG)\n"
                    "  if exited:\n"
                    "   result=os.WEXITSTATUS(status) if os.WIFEXITED(status) else 128+os.WTERMSIG(status)\n"
                    "   done=True; break\n"
                    " if done:\n"
                    "  while select.select([fd],[],[],.2)[0]:\n"
                    "   try: part=os.read(fd,4096)\n"
                    "   except OSError as e:\n"
                    "    if e.errno!=errno.EIO: raise\n"
                    "    break\n"
                    "   if not part: break\n"
                    "   data.extend(part)\n"
                    " print(json.dumps({'done':done,'result':result,'output':data.decode(errors='replace')}))\n"
                    "finally:\n"
                    " if not done:\n"
                    "  os.kill(pid,signal.SIGTERM); time.sleep(.2)\n"
                    "  try: os.kill(pid,signal.SIGKILL)\n"
                    "  except ProcessLookupError: pass\n"
                    "  os.waitpid(pid,0)\n"
                    " os.close(fd)\n"
                )
                terminal = subprocess.run([sys.executable, "-c", terminal_harness,
                                           *run_args(terminal_child, ttl=15)], env=env,
                                          capture_output=True, text=True, timeout=12)
                assert terminal.returncode == 0, terminal
                terminal_state = json.loads(terminal.stdout)
                assert terminal_state["done"] and terminal_state["result"] == 0, terminal_state
                assert "terminal-value=fsmod-tty-input" in terminal_state["output"], terminal_state
                assert_no_run_hold()

                # The TTL is a fixed safety cap, even while the child runs.
                wrapper, release, event, owner = run_live("run-ttl", ttl=3)
                expect(event, http=False, logged=False)
                wait_for(lambda: not has_owner(owner), seconds=6,
                         message="ctl run renewed a fixed TTL")
                assert wrapper.poll() is None, "TTL killed the child"
                expect(create("run-after-ttl"))
                finish_run(wrapper, release, owner)

                # Signals must reach the whole child process group, and the
                # wrapper must reap its command and release just its own hold.
                # Python 3.11 changed IntEnum.__str__; the child's signal
                # callback receives an int on every supported Python version.
                for number in map(int, (signal.SIGTERM, signal.SIGINT, signal.SIGHUP)):
                    label = "run-signal-%d" % number
                    ready = base / (label + ".ready")
                    child_signal = base / (label + ".child-signal")
                    grandchild_signal = base / (label + ".grandchild-signal")
                    grandchild_ready = base / (label + ".grandchild-ready")
                    grandchild_code = (
                        "import os,signal,sys,time; from pathlib import Path\n"
                        "def caught(n,f): Path(sys.argv[1]).write_text(str(n)); sys.exit(128+n)\n"
                        "for n in (signal.SIGTERM,signal.SIGINT,signal.SIGHUP): signal.signal(n,caught)\n"
                        "Path(sys.argv[2]).write_text(str(os.getpid()))\n"
                        "while True: time.sleep(.05)\n"
                    )
                    child_code = (
                        "import json,os,signal,subprocess,sys,time; from pathlib import Path\n"
                        "def caught(n,f):\n"
                        " Path(sys.argv[1]).write_text(str(n)); child.wait(timeout=5); sys.exit(128+n)\n"
                        "for n in (signal.SIGTERM,signal.SIGINT,signal.SIGHUP): signal.signal(n,caught)\n"
                        "child=subprocess.Popen([sys.executable,'-c',sys.argv[5],sys.argv[2],sys.argv[3]])\n"
                        "deadline=time.monotonic()+5\n"
                        "while not Path(sys.argv[3]).exists() and time.monotonic()<deadline: time.sleep(.01)\n"
                        "assert Path(sys.argv[3]).exists(); Path(sys.argv[4]).write_text(str(os.getpid()))\n"
                        "while True: time.sleep(.05)\n"
                    )
                    old_owners = {hold["id"] for hold in status()["holds"]}
                    command = [sys.executable, "-c", child_code, child_signal,
                               grandchild_signal, grandchild_ready, ready, grandchild_code]
                    wrapper = subprocess.Popen(run_args(command), env=env, stdout=subprocess.PIPE,
                                               stderr=subprocess.PIPE, text=True, start_new_session=True)
                    active_wrappers.append(wrapper)
                    wait_for(lambda: ready.exists() or wrapper.poll() is not None)
                    assert ready.exists(), wrapper.communicate(timeout=2)
                    own = {hold["id"] for hold in status()["holds"]} - old_owners
                    assert len(own) == 1, status()
                    wrapper.send_signal(number)
                    stdout, stderr = wrapper.communicate(timeout=12)
                    assert wrapper.returncode == 128 + number, (number, stdout, stderr, wrapper.returncode)
                    child_marker = child_signal.read_text() if child_signal.exists() else None
                    grandchild_marker = grandchild_signal.read_text() if grandchild_signal.exists() else None
                    evidence = {"signal": number, "child_marker": child_marker,
                                "grandchild_marker": grandchild_marker, "stdout": stdout,
                                "stderr": stderr, "wrapper_returncode": wrapper.returncode}
                    assert child_marker == str(number), evidence
                    assert grandchild_marker == str(number), evidence
                    assert not any(has_owner(item) for item in own), status()

                assert_no_run_hold()

            def expect(path, http=True, logged=True):
                if http:
                    wait_for(lambda: received.has(path), message="missing HTTP event: " + str(path))
                if logged:
                    wait_for(lambda: log_has(path), message="missing event log: " + str(path))
                # Allow a real backend delivery window before asserting absence.
                if not http or not logged:
                    time.sleep(1.25)
                assert received.has(path) == http, ("HTTP", path, http)
                assert log_has(path) == logged, ("event log", path, logged)

            def start():
                nonlocal process
                process = subprocess.Popen([binary, "-C", str(config), "-F"],
                                           env=env, stdout=output, stderr=output)
                wait_for(lambda: socket_path.exists() or process.poll() is not None)
                assert process.poll() is None, log.read_text(errors="replace")
                wait_for(lambda: subprocess.run(
                    [binary, "ctl", "status", "--json", "--socket", str(socket_path)],
                    env=env, capture_output=True, timeout=10).returncode == 0)
                assert stat.S_IMODE(socket_path.stat().st_mode) == 0o600
                time.sleep(0.5)

            def stop():
                nonlocal process
                process.terminate()
                assert process.wait(timeout=10) == 0
                process = None
                assert not pidfile.exists(), "PID lock not cleaned up"
                assert not socket_path.exists(), "control socket not cleaned up"

            try:
                start()
                state = status()
                assert not state["monitor_paused"] and not state["report_paused"], state
                assert not has_owner("com.example.absent"), state
                assert json.loads(ctl("status").stdout)["ok"] is True
                lookup = subprocess.run([binary, "-C", str(config), "ctl", "status", "--json"],
                                        env=env, capture_output=True, text=True, timeout=12)
                assert lookup.returncode == 0, lookup
                assert json.loads(lookup.stdout)["pid"] == process.pid, lookup
                saved_config = config.read_text(encoding="utf-8")
                try:
                    config.write_text("invalid_runtime_key=true\n", encoding="utf-8")
                    assert status()["pid"] == process.pid, "explicit socket loaded invalid config"
                finally:
                    config.write_text(saved_config, encoding="utf-8")
                expect(create("normal"))

                pause("com.example.full", discard=True)
                full = create("full-pause")
                expect(full, http=False, logged=False)
                resume("com.example.full")
                expect(create("after-full-resume"))
                assert not received.has(full) and not log_has(full), "paused event replayed"

                pause("com.example.report", scope="report")
                assert not status()["monitor_paused"], status()
                report_only = create("report-pause")
                expect(report_only, http=False, logged=True)
                resume("com.example.report")
                expect(create("after-report-resume"))
                assert not received.has(report_only), "report-suppressed event replayed"

                # Releasing A cannot override B; all is stronger than report.
                pause("com.example.a", scope="all")
                pause("com.example.b", scope="report")
                expect(create("overlap-all"), http=False, logged=False)
                resume("com.example.a")
                assert has_owner("com.example.b"), status()
                expect(create("overlap-report"), http=False, logged=True)
                resume("com.example.unknown")
                resume("com.example.unknown")
                assert has_owner("com.example.b"), status()
                resume("com.example.b")
                expect(create("after-overlap"))

                # Replacing a hold can also change its scope without requiring
                # a resume window between the two operations.
                pause("com.example.replace", scope="all")
                pause("com.example.replace", scope="report")
                assert not status()["monitor_paused"], status()
                assert len([hold for hold in status()["holds"]
                            if hold["id"] == "com.example.replace"]) == 1
                expect(create("scope-replaced"), http=False, logged=True)
                resume("com.example.replace")

                # Automatic expiration must resume event handling without a
                # status command or another control connection to trigger it.
                pause("com.example.autottl", ttl=1)
                time.sleep(1.3)
                expect(create("autonomous-expiration"))
                assert not has_owner("com.example.autottl"), status()

                # A repeated owner refreshes one hold rather than adding a
                # second hold. The old expiration must not resume reporting.
                pause("com.example.renew", ttl=3)
                time.sleep(1.6)
                pause("com.example.renew", ttl=3)
                time.sleep(1.6)
                assert has_owner("com.example.renew"), status()
                wait_for(lambda: not has_owner("com.example.renew"), seconds=6,
                         message="renewed lease did not expire")
                expect(create("after-expiration"))

                # Persist an unexpired hold across an ordinary daemon restart.
                pause("com.example.restart", ttl=30)
                stop()
                start()
                assert has_owner("com.example.restart"), status()
                restart_event = create("restart-paused")
                expect(restart_event, http=False, logged=False)
                resume("com.example.restart")
                expect(create("restart-resumed"))
                assert not received.has(restart_event), "restart pause leaked an event"

                # SIGKILL leaves stale PID/socket paths; the next instance must
                # recover those paths and restore the private lease state.
                pause("com.example.crash", ttl=30)
                process.kill()
                assert process.wait(timeout=10) != 0
                process = None
                start()
                assert has_owner("com.example.crash"), status()
                expect(create("crash-restored"), http=False, logged=False)
                resume("com.example.crash")
                expect(create("after-crash-resume"))
                test_run_commands()

                for args in [
                    ("unknown",),
                    ("pause", "--id", "bad/id", "--scope", "all", "--ttl", "3"),
                    ("pause", "--id", "bad id", "--scope", "all", "--ttl", "3"),
                    ("pause", "--id", "x" * 128, "--scope", "all", "--ttl", "3"),
                    ("pause", "--id", "com.example.bad", "--scope", "invalid", "--ttl", "3"),
                    ("pause", "--id", "com.example.bad", "--scope", "all", "--ttl", "0"),
                    ("pause", "--id", "com.example.bad", "--scope", "all", "--ttl", "86401"),
                    ("pause", "--id", "com.example.bad", "--scope", "all", "--ttl", "2x"),
                    ("resume",),
                    ("status", "--not-an-option"),
                ]:
                    ctl(*args, success=False)
                for request in (b"INVALID\n", b"PAUSE com.example.raw all 0 0\n",
                                b"PAUSE com.example.raw all 2 0 unexpected\n",
                                b"RESUME com.example.raw extra\n"):
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                        client.settimeout(10)
                        client.connect(str(socket_path))
                        client.sendall(request)
                        response = bytearray()
                        while not response.endswith(b"\n"):
                            part = client.recv(4096)
                            assert part, (request, response)
                            response.extend(part)
                        assert json.loads(response)["ok"] is False, (request, response)
                assert not has_owner("com.example.bad"), status()
                assert not has_owner("com.example.raw"), status()
                expect(create("after-invalid-commands"))
                stop()
                ctl("status", "--json", success=False)
                marker = base / "unavailable-daemon-must-not-execute"
                command = [sys.executable, "-c",
                           "from pathlib import Path; import sys; Path(sys.argv[1]).touch()", marker]
                result = subprocess.run(run_args(command), env=env, capture_output=True,
                                        text=True, timeout=12)
                assert result.returncode != 0 and not marker.exists(), result
                print("runtime control integration tests passed")
            finally:
                for wrapper in active_wrappers:
                    if wrapper.poll() is None:
                        wrapper.terminate()
                        try:
                            wrapper.communicate(timeout=8)
                        except subprocess.TimeoutExpired:
                            wrapper.kill()
                            wrapper.communicate(timeout=3)
                if process is not None and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
                output.close()
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
