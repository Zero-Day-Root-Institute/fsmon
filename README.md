# fsmod

[English](README.md) | [简体中文](README.zh-CN.md)

Low level filesystem monitor utility for Linux, Android, iOS and macOS.
This checkout builds `fsmod`, based on the original
[NowSecure project](https://github.com/nowsecure/fsmon).

- **Author**: pancake @ nowsecure
- **License**: MIT

Designed for

- System administrators and incident responders
- Security researchers and forensic analysts
- Developers debugging I/O-heavy applications
- Reverse engineers interested in observing filesystem access behavior

## Architecture and lifecycle

The configured service separates event collection from HTTP delivery:

```text
OS backend -> event decoding -> configuration/runtime filters -> JSON
                                                           -> event log (optional)
                                                           -> bounded queue -> HTTP worker
```

`main.c` manages startup and the event callback; `backend/` contains platform
collectors. `config.c` parses and resolves monitoring rules, `service.c` manages
the PID lock and desktop background mode, `control.c` implements local runtime
holds, and `report.c` serializes events and delivers reports.

Startup loads the configuration, selects the backend, acquires the PID lock,
starts the reporter and local control socket, then starts event collection.
Device identity, OS and kernel metadata are cached before collection;
the hostname is read when each event is serialized. SIGINT/SIGTERM stop collection and
close the backend; control, queued reports, the PID lock and configuration are
then cleaned up. The reporter drains or cancels work according to the shutdown
timeout. launchd/systemd manage restarts for installed services.

On iOS, `devfsev` opens `/dev/fsevents`, creates an event-stream descriptor with
`FSEVENTS_CLONE`, requests extended metadata, and decodes typed kernel records.
On macOS, configured mode defaults to the public `fsevapi` backend; `devfsev`
is an optional private interface. Path and identity rules filter the received
events before reporting; they do not reduce `devfsev`'s kernel subscription.
Backend availability and event detail depend on the target OS and permissions.

## Installation

Build this checkout to install `fsmod` with daemon and HTTP reporting support:

```bash
# Debian/Ubuntu: install the libcurl headers and linker metadata first.
sudo apt-get install libcurl4-openssl-dev pkg-config
make
make install PREFIX=/usr
```

On macOS, the Command Line Tools provide the system libcurl headers and library;
build with `make CURL_CFLAGS= CURL_LIBS=-lcurl` to select that library explicitly.
Desktop reporting requires libcurl 7.32.0 or newer with asynchronous DNS support.
The reporter rejects a libcurl build without asynchronous DNS at startup.
libcurl must support HTTP/HTTPS; HTTPS uses its configured CA trust store.

On macOS the build produces `fsmod-macos` and `fsmod`; on Linux it produces
`fsmod`. All usage, configuration and service examples below use the new name.
The `fsmod.rb` Homebrew recipe is retained for upstream compatibility: it builds
the original upstream release and installs its command as `fsmod`. It does not
include this checkout's daemon changes; use the source build above for those.

## Usage

### Daemon and HTTP reporting

The configured service runs on macOS, Linux, and jailbroken iOS. Desktop HTTP
delivery uses libcurl directly inside the process; iOS uses the system CFNetwork
framework and launchd. No curl command is launched. The existing interactive
CLI remains available without a configuration file. WatchOS and default Android
builds retain interactive monitoring without configured daemon reporting. See
Compilation for the Android opt-in using a target-built libcurl.

For iOS Rootful and Rootless packages, run `make ios-packages` on a Mac with Xcode
and ldid. The arm64 binary targets iOS 12.0 or newer. Packages use `/usr/sbin` and
`/var/jb/usr/sbin` respectively and share the system CFNetwork dependency. See
[the iOS deployment guide](dist/ios/README.md) for configuration and launchd
commands. Both jailbreak layouts require a privileged process and access to
`/dev/fsevents`; compatibility must be checked on the target device.

Start with `fsmod.conf.example`, then set the destination and paths for your
machine. The following example uses `/tmp`, which already exists:

```ini
report_url=http://127.0.0.1:8080/events
# report_token=your-bearer-token
watch_path=/tmp
# watch_path=/Users/your-user/Documents
# Repeat exclude_path for every existing file or directory to ignore.
# exclude_path=/tmp/cache
# exclude_path=/tmp/private
# exclude_path=/tmp/secrets.txt
# Optional process exclusions; see below for backend metadata behavior.
# exclude_process=zsh
# exclude_process=sshd
# Optional global filters on the UID supplied with each event; see below.
# watch_uid=0
# watch_uid=501
# exclude_uid=501
# Optional global filters on the GID supplied with each event; see below.
# watch_gid=0
# watch_gid=501
# exclude_gid=501
# Optional global filters on the operation process's direct parent PID:
# watch_ppid=1
# watch_ppid=1234
# exclude_ppid=1234
daemonize=true
pid_file=/tmp/fsmod.pid
log_file=/tmp/fsmod.log
log_events=false
queue_capacity=1024
report_timeout_seconds=10
report_retries=3
retry_delay_seconds=2
shutdown_timeout_seconds=15
```

```sh
cp fsmod.conf.example fsmod.conf
chmod 600 fsmod.conf
# Edit fsmod.conf before starting.
./fsmod -C ./fsmod.conf -T   # validate only; no HTTP request or monitoring
./fsmod -C ./fsmod.conf -F   # foreground; Ctrl-C stops the service
./fsmod -C ./fsmod.conf -D   # background; returns after initialization succeeds
./fsmod -C ./fsmod.conf -S   # send SIGTERM to the configured instance
```

Run one of `-F` or `-D` at a time. With explicit `-C` and neither flag,
`daemonize` controls desktop mode; implicit default-config startup stays in the
foreground, and iOS always stays in the foreground unless unsupported `-D` is requested.
The PID file is locked for the whole lifetime, including foreground mode and
reporter shutdown. `-S` returns when SIGTERM has been sent; the PID file disappears
after queue draining/cancellation and cleanup finish. Keep the same
`pid_file` when stopping an instance. Monitoring paths may have been deleted when
`-S` is used. Configuration changes take effect after a restart; there is no
SIGHUP reload.

### Runtime control

The configured daemon accepts local control commands while its monitoring
backend and launchd/systemd job remain running. This is intended for package
maintenance and other temporary reporting holds. Run commands as the daemon's
user (root for the iOS packages):

```sh
# Rootful iOS, with the packaged default configuration.
/usr/sbin/fsmod ctl status
/usr/sbin/fsmod ctl status --json
/usr/sbin/fsmod ctl pause --id com.example.package --scope all --ttl 600
/usr/sbin/fsmod ctl resume --id com.example.package

# Suppress only HTTP reports; log_events still follows the saved configuration.
/usr/sbin/fsmod ctl pause --id com.example.package --scope report --ttl 600

# Also discard reports that were pending before the hold.
/usr/sbin/fsmod ctl pause --id com.example.package --scope all --ttl 600 --discard-pending

# Pause, run an entire package installation, then release this command's hold.
/usr/sbin/fsmod ctl run --scope all --ttl 600 -- dpkg -i yourpackage.deb
# Rootless:
/var/jb/usr/sbin/fsmod ctl run --scope all --ttl 600 -- dpkg -i yourpackage.deb
```

For Rootless, use `/var/jb/usr/sbin/fsmod` with the same arguments. A pause uses a
named hold: choose a stable package identifier for `--id` (1 to 127 ASCII
letters, digits, dots, underscores or hyphens), and a TTL in whole seconds from
1 to 86400. The default scope is `all` and the default TTL is 600 seconds. At
most 64 IDs can hold a pause at once. Repeating `pause` with the same ID
replaces its scope and renews its expiration. Different IDs coexist;
`resume` releases only the named
ID and an unknown ID is a successful no-op. Monitoring/reporting resume only
when no applicable holds remain. An `all` hold takes precedence over `report`.
Holds expire automatically so a failed installer does not suppress reports
forever. Long-running maintenance must renew its hold before expiration.

| Effective state | Event stream | `log_events` | HTTP |
| --- | --- | --- | --- |
| Running | Read and process | As configured | Send |
| `all` | Read and discard | Suppressed | Suppressed |
| `report` | Read and process | As configured | Suppressed |

Pausing does not stop the daemon process or close the kernel event stream.
Events handled during a hold are dropped for the corresponding output and are
not queued for later replay. Reports queued before the hold are retained by
default and become eligible again after the last reporting hold ends;
`--discard-pending` removes them. Retained queue entries are still in memory,
not durable event storage. A paused shutdown does not send the retained queue.
On `devfsev`, raw kernel timestamps also suppress delayed events from before
the resume boundary. Backends without such timestamps use callback processing
time; an event delayed inside the backend until after resume can still appear.

A successful `pause` waits until event handling is gated and the reporter has
finished or cancelled its active transfer. Start file changes only after this
command exits successfully. A nonzero exit means that the requested operation
was not confirmed; a timeout can leave the hold active, so query `status` before
retrying. A request that already reached the HTTP server cannot be withdrawn;
the server may have stored an event even when its response was interrupted.
This control is not a transaction around filesystem changes.

Control uses a private Unix-domain socket and verifies the connecting user. It
does not add a network listener. The control state is separate from the static
configuration; commands do not reload watch roots, exclusions, or report URL.
Unexpired holds are restored before event handling after a daemon restart on
the same boot; a device reboot invalidates the old boot's holds. The state file
is the configured `pid_file` with `.ctl.state` appended and must remain private
to the daemon user. Do not delete it while maintenance holds are active. `status`, `pause`
and `resume` return JSON; `--json` on `status` is accepted for explicit script
usage. Service lifecycle/control messages remain in the normal service log
even when event logging is suppressed.

For nondefault PID files or test instances, pass the daemon socket explicitly:

```sh
# The socket name is the configured pid_file followed by .ctl.
./fsmod ctl status --json --socket /tmp/fsmod.pid.ctl
./fsmod ctl pause --id com.example.package --scope all --ttl 600 --socket /tmp/fsmod.pid.ctl
# Alternatively load the custom configuration only to discover its pid_file.
./fsmod -C ./fsmod.conf ctl status --json
```

The packaged `pid_file=/var/run/fsmod.pid` produces
`/var/run/fsmod.pid.ctl` on both iOS layouts. If the normal socket path exceeds
the platform's Unix-domain socket limit, fsmod derives a deterministic
`/tmp/fsmod-<uid>-<path-hash>.ctl` socket; the state file stays beside the PID
file. Without `--socket`, `ctl` reads the
default configuration (or `-C` override) to locate the configured instance. An
explicit `--socket` bypasses configuration loading, so it remains useful if the
configuration was edited or invalidated after startup. Successful status JSON
contains `ok`, `pid`, `monitor_paused`, `report_paused`, `queued`, `in_flight`,
`delivered`, `failed`, `dropped`, `suppressed_monitor`, `suppressed_report`, and
`holds`. Each hold contains `id`, `scope`, and `remaining_seconds`. The counters
describe the current daemon process; persisted holds do not persist events or
counters.

For integration into another deb, invoke `pause` before changing its files and
`resume` when maintenance completes, using the same ID in both scripts. Check
the exit status before starting changes, release the hold on failure paths when
possible, and keep the TTL as a fallback. Two packages using different IDs
cannot resume each other's holds. Stopping the process with `-S` is unsuitable
for a maintenance hold because launchd `KeepAlive` can restart it.

To cover the whole package-manager command, use `ctl run` before `dpkg` starts,
rather than waiting for a package's maintainer script to be invoked. It creates
a unique hold, waits for the pause acknowledgement, executes the command, waits
for it, then releases only its own hold. Existing holds from other packages
remain active. The default scope is `all`, and `--scope report` keeps local
event logging enabled according to `log_events`. `--discard-pending` also drops
the queue from before the command.

```sh
# Options belong before the required -- separator; command arguments follow it.
./fsmod -C ./fsmod.conf ctl run --scope all --ttl 1800 -- dpkg -i yourpackage.deb
./fsmod ctl run --socket /tmp/fsmod.pid.ctl --scope report --ttl 600 -- your-command --your-option
```

The separator and a command are required. `run` accepts `--scope`, `--ttl`,
`--discard-pending`, and `--socket`; it generates its own ID, so `--id` and
`--json` are not accepted. Arguments after `--` are passed directly to the
program, including any literal `--socket` or `--ttl`; no shell expansion is
performed. Invoke a shell explicitly when shell syntax is needed. The child
inherits standard input/output/error, while wrapper diagnostics go to standard
error. No control JSON is written to the command's standard output.
Interactive terminal input is supported for a foreground command. Shell job
control such as `Ctrl-Z`, `fg` and `bg` is not supported by the wrapper; avoid
suspending the command during a maintenance hold.

The wrapper does not start the command if pausing fails or the daemon cannot
be reached. It attempts to release its hold after normal exit, launch failure,
or a catchable `SIGINT`, `SIGTERM`, or `SIGHUP`; these signals are forwarded to
the child process group. It preserves the child's exit status (or `128 + signal`),
using 127 for an unavailable executable and 126 for other launch failures. If
cleanup fails after a successful child, the wrapper returns nonzero; cleanup
errors never replace an already nonzero child status.

The TTL remains a fixed safety cap and is **not renewed by `run`**. Choose a TTL
longer than the expected command duration, within the 1-to-86400-second range.
If it expires, monitoring/reporting resume even while the command continues.
If the wrapper is killed with `SIGKILL`, cleanup cannot run and the hold lasts
until its TTL expires. Descendants that detach or outlive the direct command
are outside the wrapper's command lifetime.

### Configuration

The configuration uses UTF-8 text in `key=value` format, with one setting per line. Blank lines
and lines starting with `#` are ignored. Values are unquoted; leading/trailing
spaces are trimmed, while interior spaces and `#` are literal. Unknown keys and
duplicate scalar keys are errors.
Use separate `watch_path` and `exclude_path` lines for multiple paths. For
example, add one `exclude_path` line for each directory or file to omit. Each
path must be absolute. A `watch_path` that cannot be resolved because it or a
parent is missing (`ENOENT`), or because a parent component is not a directory
(`ENOTDIR`), produces a warning and is skipped. Other path errors remain fatal.
Every `exclude_path` must still exist when the daemon starts. Path rules match
whole path components; a directory rule covers its descendants. Do not combine
paths with commas or wildcards. For each event path, the most specific matching
`watch_path` or `exclude_path` wins, regardless of configuration order. A deeper
watch restores monitoring inside an excluded directory; a deeper exclusion can
exclude part of that watch again. At the same path, exclusion wins. For example:

```ini
watch_path=/Library
watch_path=/Library/dpkg/info
exclude_path=/Library/dpkg
```

This monitors `/Library`, excludes `/Library/dpkg`, and restores monitoring of
`/Library/dpkg/info` and its descendants. Startup and `-T` reject a configuration
if every effective watch is still excluded. A deeper watch restored inside a
parent exclusion counts as monitored scope. Stop/control commands remain
available with an otherwise fully excluded config.

At least one `watch_path` entry is required, and the limit of 64 counts all
configured entries, including skipped paths. Existing roots are canonicalized
with `realpath`; nested watches are allowed, but duplicate canonical watch paths
are rejected. Collection uses the outermost nonoverlapping roots while all
configured watch rules remain active. The Linux `fanotify` backend keeps every
explicit root so nested mount points are also marked. If every
watch path is skipped, normal startup succeeds with `roots=0` and remains idle,
and `-T` succeeds with warnings if the rest of the configuration is valid.
Creating a skipped path later does not add a watch; restart fsmod to include it.

Use optional `watch_events` once after a `watch_path` to select events for that
path. It belongs to the most recent `watch_path`, even if comments or unrelated
settings appear between them. Each new `watch_path` starts with `all`; omitting
`watch_events` keeps that default, including for nested watches. The most
specific matching watch supplies the event selection; parent and child event
selections are not inherited or combined. For example (uncomment the desired pairs):

```ini
# watch_path=/private/etc
# watch_events=create,modify
# watch_path=/bin
# watch_events=all
# Or use a group together with an exact event name:
# watch_path=/sbin
# watch_events=create,FSE_CONTENT_MODIFIED
```

| `watch_events` token | Accepted event types |
| --- | --- |
| `all` | Every event type provided by the selected backend; must appear alone. |
| `create` | `FSE_CREATE_FILE`, `FSE_CREATE_DIR` |
| `modify` | `FSE_CONTENT_MODIFIED`, `FSE_STAT_CHANGED`, `FSE_CHOWN`, `FSE_XATTR_MODIFIED`, `FSE_XATTR_REMOVED`, `FSE_FINDER_INFO_CHANGED` |
| `delete` | `FSE_DELETE` |
| `rename` | `FSE_RENAME` |
| `clone` | `FSE_CLONE` |
| `exchange` | `FSE_EXCHANGE` |

Tokens are case-sensitive and comma-separated; groups can be mixed with exact
event names. Supported exact names are `FSE_CREATE_FILE`, `FSE_CREATE_DIR`,
`FSE_DELETE`, `FSE_STAT_CHANGED`, `FSE_RENAME`, `FSE_CONTENT_MODIFIED`,
`FSE_EXCHANGE`, `FSE_FINDER_INFO_CHANGED`, `FSE_CHOWN`, `FSE_XATTR_MODIFIED`,
`FSE_XATTR_REMOVED`, `FSE_DOCID_CREATED`, `FSE_DOCID_CHANGED`,
`FSE_UNMOUNT_PENDING`, `FSE_CLONE`, `FSE_OPEN`, `FSE_CLOSE`,
`FSE_CLOSE_WRITABLE`, and `FSE_UNKNOWN`. Repeated tokens are harmless. Unknown
names, empty items (including a trailing comma), mixing `all` with other tokens,
using `watch_events` without an earlier `watch_path`, or specifying it twice
for the same path are configuration errors. Rules for missing/skipped paths are
still validated and never change the rule of a preceding effective root.

For two-path events such as rename or clone, each path is resolved independently
using the most specific rule. If either path is finally excluded, the whole event
is omitted; a path outside all rules does not count as excluded. Otherwise, at
least one included path's most specific watch must allow the event type.
These rules filter HTTP reporting and `log_events`, not kernel subscriptions.
`all` cannot make a backend produce unavailable events: it does not enable read
tracking or recover the source of a libc read/write copy. Restart after edits.

Repeat `exclude_process` to ignore events from particular process names:

```ini
# iOS/devfsev example; match the exact value shown in data.proc.
exclude_process=zsh
exclude_process=sshd
exclude_process=SpringBoard
```

Matching is exact and case-sensitive; duplicate names are deduplicated. These
are process names, not PIDs, executable paths, glob patterns, or substrings.
Excluding a process does not automatically exclude its children. No process
names are excluded by default. The kernel still collects events; fsmod discards
matching events in its callback before both the HTTP queue and `log_events`
output. Restart to apply changes.

The setting is accepted with every backend. `devfsev` and Linux `fanotify`
usually provide originating-process metadata, while `fsevapi` and `inotify`
may leave it unavailable. A process name can also be unavailable (for example,
if the process has exited) or truncated by the OS. Match the actual
`data.proc` value. Events whose process name is unavailable are kept, subject
to the other filters.

Use `watch_uid` to accept only selected event UIDs, or `exclude_uid` to omit
selected event UIDs. Each line contains one decimal value from `0` to
`4294967295`; repeat the key for multiple values. Names such as `root`/`mobile`,
PIDs, device UUIDs, comma-separated lists, signs and wildcards are not accepted.
For example, uncomment only the rules required for the deployment:

```ini
# Accept events with either UID 0 or UID 501:
# watch_uid=0
# watch_uid=501
# Optional exclusion; if combined with the lines above, only UID 0 remains:
# exclude_uid=501
```

These rules match the backend-supplied value in `data.uid`. On `devfsev`,
`FSE_ARG_UID`/file-info metadata identifies the file's owner UID; it is not a
guarantee of the UID of the process performing the operation. fsmod does not
substitute its own `getuid()`, a later `stat()` result or inferred process
credentials. The current other backends do not supply a usable event UID.
A numeric zero in JSON alone does not prove root ownership or a root process: it
can also represent unavailable metadata.

No UID rules are enabled by default. Without either key, known and unavailable
UIDs remain eligible under the other filters. Once any UID rule is configured,
events without a backend-supplied UID are discarded. With `watch_uid`, the UID
must match at least one entry; with only `exclude_uid`, every known UID except
the excluded entries is eligible. `exclude_uid` always wins when a value occurs
in both lists. Duplicate values are deduplicated, and there is no fixed entry
count limit apart from available memory.

UID rules are global and combine with path, event-type and process filters:
an event must pass all of them. Matching happens before both HTTP queueing and
`log_events`; the kernel continues collecting events. Restart after changes.
The report JSON schema remains unchanged.

Use `watch_gid` to accept only selected event GIDs, or `exclude_gid` to omit
selected event GIDs. Each line contains one decimal value from `0` to
`4294967295`; repeat the key for multiple values. These rules match the
backend-supplied `data.gid` value. On `devfsev`, it is the file owner's group
GID from event metadata, not the operation process GID or its supplementary
groups. Current other backends do not supply a usable event GID.

No GID rules are enabled by default. With `watch_gid`, the GID must match at
least one entry; with only `exclude_gid`, every known GID except the excluded
entries is eligible. `exclude_gid` takes precedence when a value occurs in
both lists. Duplicate values are deduplicated, and there is no fixed entry
count limit apart from available memory. Once any GID rule is configured,
events without a backend-supplied GID are discarded.

UID and GID rule groups are combined with AND semantics: an event must pass
the UID rules and the GID rules, as well as path, event-type and process
filters. They are not paired by line and do not form an OR across groups.
GID rules are global and apply before HTTP queueing and `log_events`; kernel
collection continues. Restart after changes. The report JSON schema remains
unchanged.

Use `watch_ppid` to accept only selected direct parent process IDs, or
`exclude_ppid` to omit them. Each line accepts one decimal value from `1` to
`2147483647`; signs, names, comma-separated lists and wildcards are invalid.
Repeat either key for multiple values; duplicates are deduplicated, with no
fixed entry count limit apart from available memory. Without either key, PPID
does not restrict events. A nonempty `watch_ppid` list is an allowlist, and
`exclude_ppid` takes precedence. Once any PPID rule is configured, unavailable
or nonpositive PPIDs are discarded.

These rules match `data.ppid`: the operation process's direct parent PID
resolved when fsmod consumes the event. Apple `devfsev` and Linux `fanotify`
attempt to resolve it; daemon backends `fsevapi` and `inotify` do not provide
PPID information. It is not a parent name, an ancestor, a file UID/GID, or a
stable process identity. Process exit and PID reuse can affect the result.
For example, `watch_ppid=1` selects events whose resolved direct parent PID is
1; it does not select all descendants of PID 1.

PPID rules combine with UID, GID, path, event-type and process rules using AND:
every filter must pass. They apply before HTTP queueing and `log_events`;
kernel collection continues. Restart after changes. The report JSON schema
remains unchanged.

| Setting | Meaning / default |
| --- | --- |
| `report_url` | Required HTTP/HTTPS endpoint; each event is a JSON POST. HTTPS verifies the server certificate. Redirects are not followed. |
| `report_token` | Optional bearer token; protect the configuration with mode `0600`. The token stays in process memory and is sent as an HTTP Authorization header; it is not written to a temporary file or passed to another process. |
| `watch_path` | At least one absolute path entry is required; repeat up to 64 configured entries. Missing paths (`ENOENT`/`ENOTDIR`) warn and are skipped, including during `-T`; all skipped roots leave an idle `roots=0` service. Existing regular files/directories are canonicalized; nested watches are allowed, but duplicate canonical paths are rejected. The most specific path rule wins. Restart after creating skipped paths. |
| `watch_events` | Optional event selection for the most recent `watch_path`, at most once per path; default `all` resets at each new path. The most specific watch's selection applies without inheritance or union. Comma-separated groups and exact `FSE_*` names filter HTTP reporting and event logs. |
| `exclude_path` | Existing absolute path to exclude recursively unless a deeper watch restores monitoring. The most specific watch/exclude rule wins; exclusion wins at the same path. A final exclusion on either side of a rename or clone drops the whole event. |
| `exclude_process` | Optional repeatable exact, case-sensitive event process name to exclude from HTTP reporting and event logs. Backends without process metadata leave those events eligible. Default: no exclusions. |
| `watch_uid` | Optional repeatable decimal event UID (`0..4294967295`) allowlist; default: no UID restriction. When configured, only listed, available UIDs pass. |
| `exclude_uid` | Optional repeatable decimal event UID (`0..4294967295`) exclusion; takes precedence over `watch_uid`. Any UID rule rejects unavailable UID metadata. |
| `watch_gid` | Optional repeatable decimal event GID (`0..4294967295`) allowlist; default: no GID restriction. When configured, only listed, available GIDs pass. |
| `exclude_gid` | Optional repeatable decimal event GID (`0..4294967295`) exclusion; takes precedence over `watch_gid`. Any GID rule rejects unavailable GID metadata. |
| `watch_ppid` | Optional repeatable decimal direct parent PID (`1..2147483647`) allowlist; default: no PPID restriction. Any PPID rule rejects unavailable or nonpositive PPIDs. |
| `exclude_ppid` | Optional repeatable decimal direct parent PID (`1..2147483647`) exclusion; takes precedence over `watch_ppid`. Matches `data.ppid` resolved when consuming the event. |
| `backend` | macOS default `fsevapi`; iOS default `devfsev`; Linux default `inotify`. `devfsev` and Linux `fanotify` require appropriate privileges. |
| `daemonize` | Desktop configuration default `true`; iOS default `false`. No-argument/default-config startup stays in the foreground for the supervisor. iOS stays in the foreground unless `-D` is explicitly requested, which is unsupported. `-F` forces foreground mode; desktop `-D` forces background mode. |
| `pid_file`, `log_file` | Default `/tmp/fsmod.pid`, `/tmp/fsmod.log`; absolute filenames in existing directories. Use private writable directories for a deployed service. PID/log files must be different. |
| `log_events` | `false` by default; `true` also writes each accepted event as JSON to stdout. In background mode stdout/stderr append to `log_file`; in foreground mode the caller manages them. |
| `queue_capacity` | Pending event limit, default `1024`, range `1..1000000`. A full queue drops new events and records a counter. |
| `report_timeout_seconds` | Per-attempt connection/transfer timeout, default `10`, range `1..3600`. |
| `report_retries` | Additional attempts after the initial request, default `3`, range `0..100`. |
| `retry_delay_seconds` | Fixed delay between attempts, default `2`, range `0..3600`. |
| `shutdown_timeout_seconds` | Queue drain/cancellation target on SIGTERM/SIGINT, default `15`, range `1..3600`. At the deadline the reporter cancels the active transfer and discards remaining queued events; final cleanup may take longer. |

The shutdown setting is not an absolute process exit deadline. Older libcurl
versions may wait for a threaded system DNS lookup during cleanup; resolver or
TLS cleanup can extend shutdown. When supported by the build and runtime
libcurl (7.87.0 or newer), the reporter enables best-effort quick cancellation
during shutdown; this still does not guarantee an exact exit time.

Directory roots include descendants. A file root watches its parent and filters
the result, so an atomic replacement of the same filename remains observable.
Symlinked roots are resolved once; the service does not follow later symlink
retargeting. Linux directory watches do not follow nested symlink directories.
Daemon mode subscribes to changes rather than reads, so delivering a report does
not create a new report just by reading libraries or certificates. Own PID/log
files and process events are filtered. Redirect foreground output away
from monitored paths, or explicitly exclude its destination.

On macOS, `fsevapi` uses FSEventStream file events. Notifications can be coalesced
and do not provide the originating PID, process name, or a paired rename target.
The legacy `devfsev` backend uses a private kernel interface and needs device/OS
permissions; it is optional and needs validation on the target OS. On Linux,
`inotify` reports directory/file changes without originating process metadata.
The optional `fanotify` daemon backend reports content modifications and writable
closes using the existing mount-mark API; it does not provide the same
create/delete/rename coverage as `inotify`. Events may be lost when an OS queue
overflows; warnings are logged. A removed or renamed directory root can require
a restart to restore full monitoring.

`devfsev` requests a kernel queue depth of 4096 events and reads in 64 KiB
batches. These are build-time settings, separate from the HTTP
`queue_capacity`. A `kernel fsevents queue dropped events` warning means the
kernel reported a gap in the event stream; monitoring continues, but the
missing events cannot be recovered. Larger buffers help with bursts and do
not guarantee lossless delivery under sustained load.

Reports are sent sequentially by a bounded background queue. Only HTTP `200` is
success; all other statuses, including `201` and `204`, fail. A successful response
may have a nonempty body; the reporter discards the body without interpreting it.
The worker reuses its libcurl handles and eligible HTTP connections across reports.
A failed request
is retried according to the configuration and then
dropped. This is best-effort delivery: there is no disk spool or replay after a
crash, and a retry may duplicate a request already accepted by the server.
Network failures, full queues and shutdown totals are logged. Backend
initialization success does not imply the HTTP server is reachable.

Each HTTP report has the envelope `{"source":"fsmod","data":{...}}`.
`source` is the fixed program identifier `fsmod`; it identifies the reporting
application, while `data.uuid` identifies the host or OS installation. No extra
configuration is required. `data` is a JSON object containing all host and
event fields, not a JSON-encoded string. Receivers must now read
`data.filename`, `data.uuid`, `data.type` and the other nested fields instead of
their former top-level locations. Configured `log_events=true` output uses the
same envelope. Traditional interactive CLI `-j`/`-J` output keeps its existing
format. The HTTP endpoint, POST method, Content-Type and bearer authentication
are unchanged. Only HTTP `200` confirms delivery; other statuses use the existing
retry configuration.

For a ready-to-configure receiver that stores complete reports in Redis DB 3,
see the [OpenResty deployment guide](server/openresty/README.md) (Chinese).

Example request body. All values below are illustrative, not values read from
your computer. Event metadata depends on the selected backend; zero/null may
mean unavailable, so do not infer a root process from `data.uid=0`:

```json
{
  "source": "fsmod",
  "data": {
    "hostname": "my-mac",
    "uuid": "01234567-89AB-CDEF-0123-456789ABCDEF",
    "os_name": "macOS",
    "os_version": "15.0",
    "kernel_version": "24.0.0",
    "backend": "fsevapi",
    "observed_at": "2026-10-26T08:00:00.123Z",
    "type": "FSE_CONTENT_MODIFIED",
    "filename": "/private/tmp/example.txt",
    "newfile": null,
    "proc": null,
    "event": null,
    "pid": 0,
    "ppid": 0,
    "uid": 0,
    "gid": 0,
    "inode": 0,
    "mode": 0,
    "dev_major": 0,
    "dev_minor": 0,
    "kernel_timestamp": 0
  }
}
```

The following host fields are strings inside `data`, or `null` when the metadata
source cannot be read. Per-event process and file metadata is also inside
`data`:

| Field | Meaning and source |
| --- | --- |
| `data.uuid` | Device/installation identifier. iOS reads the device's `UniqueDeviceID` (UDID) through the native MobileGestalt API; a UDID may be 25 or 40 characters and is not necessarily an RFC 4122 UUID. macOS uses the uppercase host UUID from `gethostuuid()`. Linux uses the lowercase machine ID from `/etc/machine-id`, falling back to `/var/lib/dbus/machine-id`. Failed reads produce `null`; no random UUID or hostname is substituted. |
| `data.os_name` | `iOS` on iOS; `macOS` on macOS; distribution `NAME` from `/etc/os-release` on Linux, falling back to `/usr/lib/os-release`. If release metadata is unavailable, Linux can use the system name from `uname`. |
| `data.os_version` | iOS/macOS product version, queried through `kern.osproductversion`, with `ProductVersion` in `/System/Library/CoreServices/SystemVersion.plist` as a fallback for older systems. Linux uses `VERSION_ID` from the same os-release source as `data.os_name`. |
| `data.kernel_version` | Kernel release from `uname`. On macOS this is the Darwin release, which is a different version from the macOS product version in `data.os_version`. |

`data.uuid` identifies the host or OS installation; numeric `data.uid` remains
the UNIX user ID supplied by the event backend. The identifier is not a user ID, an
authentication token, or a guarantee of a globally unique physical device.
In particular, Linux machine IDs can be duplicated by cloned images and can
change after reinstallation or regeneration. Hardware replacement and virtual
machine cloning can also affect host identity. Account for these cases when
grouping reports from devices; hostname changes do not generate a replacement
identifier in fsmod.

Device identity, OS and kernel metadata are collected once per process, when the first reporter starts,
before filesystem monitoring begins. The cached values are reused for every
event; the hostname is read with `gethostname()` during each serialization.
There is no per-event shell command or metadata-file lookup. Restart the
service after an OS upgrade or identity change to refresh them. Missing fields
stay `null` for that process lifetime. The iOS UDID lookup uses the system
`/usr/lib/libMobileGestalt.dylib` for both Rootful and Rootless layouts, without
launching a subprocess. The signed binary requests access to `UniqueDeviceID`
through its MobileGestalt entitlement; access still depends on the device and
jailbreak. A failed lookup emits a warning in the service log and leaves
`data.uuid` as `null`.

For the `devfsev` backend, `data.kernel_timestamp` preserves the event's raw
64-bit `mach_absolute_time()` tick value from the kernel's final `FSE_ARG_INT64`
argument. It is not a Unix timestamp, seconds, or milliseconds. Use `observed_at`
for the UTC time at which fsmod serialized the event. Backends that do not supply
a kernel timestamp leave this field at zero; fsmod does not fabricate one from
the current clock. `data.inode` preserves the kernel's unsigned 64-bit inode
number. Readers must avoid rounding these integers through floating-point JSON
representations.

`devfsev` reports filesystem changes, rather than every file read or a complete
record of a program's copy commands. An ordinary byte copy can therefore appear
as `FSE_CREATE_FILE`, `FSE_CONTENT_MODIFIED` and metadata changes for the
destination, with `data.newfile` set to `null`. Reading the source does not
provide a source-to-destination relationship in these events. fsmod does not
guess that relationship from matching process IDs, event times or file contents.
For a two-path event supplied by the kernel, `data.filename` is the first path
and `data.newfile` is the second path; a rename uses the old and new paths.
The backend also subscribes to `FSE_CLONE` (event type 14). When the kernel
emits a native clone event, `data.filename` is the source and `data.newfile` is
the destination. This preserves the kernel's explicit copy relationship; it
does not make source paths available for ordinary byte-copy implementations.

### launchd and systemd

Service templates are provided in `dist/launchd/com.apple.fsmod.plist` and
`dist/systemd/com.apple.fsmod.service`. Both run `/usr/local/sbin/fsmod` without arguments;
the supervisor handles detachment, output and restarts. Install the binary with
`sudo make install PREFIX=/usr/local` and edit a mode-0600 `/etc/fsmod.conf` first.
For a root service, set `pid_file=/var/run/fsmod.pid` and
`log_file=/var/log/fsmod.log`; ensure configured directories exist. In launchd
mode the log setting should match `StandardOutPath`/`StandardErrorPath`.
The local daemon version is `1.0.0`; the package identifier and service name
are `com.apple.fsmod`. The optional Homebrew compatibility recipe still points
to its original upstream release, rather than this checkout's daemon.
No-argument startup reads the default configuration and stays in the foreground
for the supervisor. On Rootful iOS and desktop hosts the default is
`/etc/fsmod.conf`; on Rootless iOS it is the matching jailbreak prefix's
`etc/fsmod.conf`. `-T`, `-S`, `-F` and `-D` also accept that default without `-C`.
The executable installs under `sbin` only; no `bin/fsmod` compatibility entry is
created. On Rootful iOS, start it directly with `/usr/sbin/fsmod`.

macOS LaunchDaemon installation:

```sh
sudo install -m 0644 dist/launchd/com.apple.fsmod.plist /Library/LaunchDaemons/
sudo launchctl bootstrap system /Library/LaunchDaemons/com.apple.fsmod.plist
sudo launchctl bootout system /Library/LaunchDaemons/com.apple.fsmod.plist
```

Linux systemd installation:

```sh
sudo install -m 0644 dist/systemd/com.apple.fsmod.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now com.apple.fsmod.service
sudo systemctl stop com.apple.fsmod.service
```

Use the supervisor's stop command for a managed service. In particular launchd's
`KeepAlive` restarts a process stopped directly with `-S`. Increase the service
stop timeout if you increase `shutdown_timeout_seconds` beyond the template's
30-second timeout.

### Tests

On a Linux or macOS host with a C compiler, Python 3 and the libcurl development
files described in Installation:

```sh
make test
```

This builds fsmod, runs configuration, reporter and process-lifecycle unit tests,
then exercises foreground/background operation against a loopback HTTP server.
The reporter unit test uses a loopback HTTP server to check retries, authentication,
JSON escaping, queue overflow and bounded shutdown without external networking.
Linux and macOS CI run the same suite.

### Interactive CLI

The tool retrieves file system events from a specific directory and shows them in colorful format or in JSON.

It is possible to filter the events happening from a specific program name or process id (PID).

```console
$ ./fsmod -h
Usage: ./fsmod-macos [-Jjc] [-a sec] [-b dir] [-B name] [-p pid] [-P proc] [path]
 -a [sec]  stop monitoring after N seconds (alarm)
 -b [dir]  backup files to DIR folder (EXPERIMENTAL)
 -B [name] specify an alternative backend
 -c        follow children of -p PID
 -f        show only filename (no path)
 -h        show this help
 -j        output in JSON format
 -J        output in JSON stream format
 -n        do not use colors
 -L        list all filemonitor backends
 -p [pid]  only show events from this pid
 -P [proc] events only from process name
 -v        show version
 [path]    only get events from this path
Examples:
 fsmod /data
 fsmod -J / | jq -r .filename
 fsmod -B fanotify /home
$
```

## 🔍 Key Features of fsmod

`fsmod` is a low-level, cross-platform filesystem monitor designed for developers, forensic analysts, and reverse engineers. It works by hooking into the OS kernel's tracing facilities or file notification APIs.

### ✅ Supported Platforms

- **Android**: via `inotify`, `fanotify` is not always supported
- **Linux**: via `inotify` and `fanotify`
- **macOS**: using `kdebug`, `FSEvents`, `kqueue`, and `/dev/fsevents`
- **Jailbroken iOS**: via `/dev/fsevents` (`devfsev`), with Rootful and Rootless packages

### Core Capabilities

- **Real-Time File Monitoring**
  Detects and reports file operations such as creation, deletion, modification, attribute changes, and renames in real-time.

- **Multi-Backend Support**

  Uses a platform default backend and allows an explicit selection with `backend`
  in configured mode or `-B` in interactive mode:
  - `inotify`, `fanotify` (Linux)
  - `fsevapi` (macOS); `kdebug`, `devfsev`, `kqueue` (iOS / macOS)

  The list of backends can be listed with `fsmod -L`.

- **Process-Level Insights**
  Includes process names, PIDs and parent PIDs when the backend can resolve them.
  With `devfsev`, event UID/GID describe file ownership rather than the acting
  process's identity.

- **Recursive Monitoring**
  Monitors entire directory trees recursively, dynamically adding new directories.

- **JSON Output Format**
  Supports structured logging in JSON or JSON stream mode for easy integration with other tools (e.g. `jq`, `ELK`, etc).

- **Filename Filtering & Formatting**
  Optionally strips full paths, shows only filenames, and colorizes output based on event type.

- **Selective Monitoring**
  Filter events by:
  - Specific process name (`-P`)
  - Specific PID (`-p`)
  - Child processes (`-c`)
  - Files under a given path

- **Backup on Event**
  Automatically copies affected files to a backup directory when changes are detected (`-b`).

- **Timestamping**
  Adds timestamps to each event to facilitate forensic analysis.

- **Minimal Dependencies**
  Written in portable C. Interactive monitoring uses system libraries; daemon
  HTTP reporting on desktop Linux/macOS links libcurl directly.

- **Graceful Shutdown & Signal Handling**
  Handles `SIGINT`, `SIGTERM`, and `SIGALRM` to allow clean exits and timed monitoring sessions.

### 🧪 Event Types Tracked

Examples of events that `fsmod` can detect:

- `FSE_CREATE_FILE`, `FSE_CREATE_DIR`, `FSE_DELETE`, `FSE_RENAME`
- `FSE_CONTENT_MODIFIED`, `FSE_STAT_CHANGED`, `FSE_CHOWN`
- `FSE_XATTR_MODIFIED`, `FSE_XATTR_REMOVED`, `FSE_FINDER_INFO_CHANGED`
- `FSE_CLONE`, `FSE_EXCHANGE`

Coverage varies by backend. Configured daemon mode observes changes; it does
not trace every file read or recover ordinary byte-copy source paths. Selecting
`watch_events=all` cannot enable events the backend does not provide.

## Compilation

Native desktop builds use:

```bash
$ make
```

On a Mac with the iPhoneOS SDK and ldid, use `make ios` or `make ios-packages`
for the current arm64 iOS targets. See [iOS packaging](dist/ios/README.md).

Native desktop builds discover libcurl with `pkg-config`, then `curl-config`,
with `-lcurl` as the library fallback. Set `CURL_CFLAGS` and `CURL_LIBS` explicitly
for a nonstandard native installation. These are build-time helpers only; runtime
reporting never launches either executable.

`make ios`, `make wch`, and default Android builds do not link libcurl. iOS uses
CoreFoundation/CFNetwork for HTTP; watchOS retains interactive monitoring. Android
recipes set `FSMOD_NO_REPORT=1`; the monitor remains available, but starting the
configured daemon reporter fails with an unsupported-build error.

The retained `make android` / `android-shell.sh` path needs toolchain adaptation:
the script depends on `make_standalone_toolchain.py` and hardcodes `--api=28`,
while the Makefile passes `ANDROID_API` as a bare compiler argument. These
recipes are not a verified one-step build with a modern NDK.

After adapting the target toolchain, opt in with `ANDROID_REPORT=1` and explicit
`ANDROID_CURL_CFLAGS` / `ANDROID_CURL_LIBS`. Supply an HTTP/HTTPS-capable libcurl
(including its dependencies) compiled for the selected NDK ABI/API, with
asynchronous DNS support. `ANDROID_CURL_LIBS` is required when opting in.

Use the correct target dependency linker flags and deploy the target shared
libraries when not linking statically. The Android opt-in deliberately does not
discover host libraries with `pkg-config` or `curl-config`. Device permissions and
backend support still determine which monitoring/service features are usable.

## License

This tool is free software developed by [NowSecure](https://nowsecure.com) and distributed under the MIT license.

You can reach out **Sergi Alvarez** via email [pancake@nowsecure.com](mailto:pancake@nowsecure.com)
