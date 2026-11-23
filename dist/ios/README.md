# iOS jailbreak packages

This directory packages the already-built iOS `fsmod` executable for Rootful
and Rootless jailbreaks. Rootless refers to the jailbreak's installation layout;
the launch daemon still runs as `root`. It does not make the private filesystem
event device available to an unprivileged process. Both packages use `devfsev`,
whose availability and entitlement requirements depend on the device's kernel
and jailbreak. A successful build is not a device compatibility test.
The current version is `1.0.0`; the package identifier and launchd label are
both `com.apple.fsmod`. The executable and configuration remain named `fsmod`.
The report keeps numeric event `uid`/`gid` and includes iOS system/kernel versions.
`data.uuid` contains the device's `UniqueDeviceID` (UDID), read through the native
MobileGestalt API. A UDID may be 25 or 40 characters and is not necessarily an
RFC 4122 UUID. Both package layouts use the system
`/usr/lib/libMobileGestalt.dylib`; no external command or random substitute is
used. The lookup runs once per process before monitoring starts and its result
is cached for subsequent events. The signed binary requests
`com.apple.private.MobileGestalt.AllowedProtectedKeys` access to `UniqueDeviceID`;
actual availability depends on the device and jailbreak. If the lookup fails,
fsmod logs a warning and reports `data.uuid` as `null` for that process lifetime.
Restart after resolving an access problem to retry. `data.uid` remains the
numeric UNIX user ID of the event, separate from this device identifier.

`data.kernel_timestamp` preserves the kernel's raw `mach_absolute_time()` ticks
from the event's final `FSE_ARG_INT64`; it is not Unix seconds or milliseconds.
`observed_at` is the UTC time at which fsmod serialized the event. The parser
reads every typed argument using its own 16-bit payload length and preserves
64-bit inode values, including events containing two paths.

The backend reports filesystem changes, not every source-file read. An
ordinary byte copy can produce only destination create, content and metadata
events; those events do not contain the copy source and keep `data.newfile`
as `null`. fsmod does not guess copy relationships from the process ID, event
timing or file contents. Two-path kernel events use `data.filename` for the
first path and `data.newfile` for the second; rename events contain the old
and new paths.
The backend also subscribes to `FSE_CLONE` (event type 14): a native clone
event reports the source in `data.filename` and destination in `data.newfile`.
Whether a copy produces this event depends on the copy implementation and the
kernel. Ordinary byte copies still do not provide source paths.

The backend requests a kernel event queue depth of 4096 and uses a 64 KiB
receive buffer to accommodate bursts. These build-time values are separate
from the HTTP `queue_capacity`. The warning `kernel fsevents queue dropped
events` means the kernel reported lost events; fsmod continues reading, but
cannot replay those missing events. Sustained load can still cause losses.

To distinguish missing clone delivery from an ordinary copy implementation,
build `make ios-copy-probe` on the Mac, transfer
`out/ios/fsmod-copy-probe-arm64` to the phone as `/tmp/fsmod-probe`, and run
`chmod 755 /tmp/fsmod-probe` followed by `/tmp/fsmod-probe /sbin` as root when
`/sbin` is a watched directory. The probe creates a unique
`/sbin/fsmod-probe-XXXXXX` directory, a small source file, an explicit
`clonefile()` result, and a separate read/write copy. Existing files are never
overwritten. A failed clone prints its errno and does not fall back to copying;
the read/write test still runs. Files are retained for inspection.

With clone success and an active clone-capable daemon, inspect local event logs
and Redis for `FSE_CLONE`, with `source.txt` as `filename` and `cloned.txt` as
`newfile`. The `byte-copy.txt` result may have only destination mutation events.
This is a diagnostic, not evidence that a different application uses clonefile.
If clone succeeds but its event is missing, check the installed package,
launchd executable/PID, event-drop warnings, and source/destination exclusions.
Package upgrades stop the old service and load the newly installed executable.
`fsmod -v` currently shows the base version, so use
`dpkg-query -W com.apple.fsmod` to check the package revision.

| Item | Rootful | Rootless |
| --- | --- | --- |
| Debian architecture | `iphoneos-arm` | `iphoneos-arm64` |
| Executable | `/usr/sbin/fsmod` | `/var/jb/usr/sbin/fsmod` |
| Configuration | `/etc/fsmod.conf` | `/var/jb/etc/fsmod.conf` |
| Launch daemon | `/Library/LaunchDaemons/com.apple.fsmod.plist` | `/var/jb/Library/LaunchDaemons/com.apple.fsmod.plist` |
| Maintainer-script shell | `/bin/sh` | `/var/jb/bin/sh` |
| Maintainer-script launchctl | `/bin/launchctl` | `/var/jb/bin/launchctl` |
| PID file | `/var/run/fsmod.pid` | `/var/run/fsmod.pid` |
| Log file | `/var/mobile/Library/Logs/fsmod/fsmod.log` | `/var/mobile/Library/Logs/fsmod/fsmod.log` |

The Debian architecture names describe package-manager layouts. Both packages
contain a physical-device **arm64** executable; neither supports 32-bit devices.
The two variants have the same package identifier and are alternatives, not
packages to install together.

## Build and package on the Mac

Use the root Makefile's iOS build target with daemon reporting enabled, then
package the signed output from the repository root:

```sh
make ios
python3 dist/ios/package.py --binary fsmod-ios --variant both
```

The Makefile builds `config.c`, `service.c`, `control.c` and the CFNetwork reporter for iOS,
and signs the executable with the supplied `entitlements.plist`. No libcurl or
external curl binary is required on the device. The minimum iOS version is 12.0. Do not use an
older build that disables reporting on iOS. Check the resulting executable's
linked libraries and minimum iOS version on the Mac:

```sh
file fsmod-ios
otool -L fsmod-ios
otool -l fsmod-ios
```

Output defaults to `out/ios/`:

```text
com.apple.fsmod_<version>_iphoneos-arm.deb
com.apple.fsmod_<version>_iphoneos-arm64.deb
```

The script uses only Python 3's standard library. No `dpkg-deb`, GNU tar, root
privileges, or device access is required. It validates the thin arm64 Mach-O
header, physical iOS platform load command, and presence of a code-signature
load command. This does not verify a signature's trust or runtime entitlements.
It preserves the binary bytes and does not re-sign or install anything.

`--variant rootful` or `--variant rootless` creates only that package. Other
options are `--version`, `--package-id`, `--output-dir`, and
`--source-date-epoch`. With the same inputs and `SOURCE_DATE_EPOCH` (default 0),
package timestamps, member order, ownership, modes, and gzip headers are stable.
Each generated package's SHA-256 is printed. If changing package identifiers,
also change the launchd label and paths before installation to avoid conflicts.
The default package declares `Conflicts`/`Replaces` for `com.nowsecure.fsmod`.
Its pre-install script stops the legacy and current launchd labels before replacing
files, so an existing job cannot retain the old executable path.
Back up the existing configuration before this package-ID migration and retain
the local version if dpkg asks about a conffile; do not purge the old package.
The post-install script validates the default configuration and automatically
enables and loads `com.apple.fsmod`. Invalid configuration leaves package
configuration incomplete and prints an error; fix the configuration and run
`dpkg --configure com.apple.fsmod` to retry.
Rootful upgrades unload the previous `/System/Library/LaunchDaemons/com.apple.fsmod.plist`
and remove that obsolete file after configuration validation, before loading
the restored `/Library/LaunchDaemons/com.apple.fsmod.plist`.

The config is marked as a Debian conffile so package upgrades can preserve local
edits. The package includes removal and upgrade scripts that unload the service.
Launchd templates have `Disabled=false`, `RunAtLoad=true` and `KeepAlive=true`,
so the service starts on installation and on subsequent boots.
Maintainer scripts use an absolute, variant-specific shell and `launchctl`
path instead of depending on the package manager's `PATH`. These paths follow
the [Theos rootless prefix](https://theos.dev/docs/rootless) and the primary
Procursus bootstrap definitions for
[shell](https://github.com/ProcursusTeam/Procursus/blob/master/makefiles/dash.mk)
and [launchctl](https://github.com/ProcursusTeam/Procursus/blob/master/makefiles/launchctl.mk).

The rootful data archive does not claim ownership of the existing `/System`
directory hierarchy. The data archive does not claim ownership of system symlink
anchors `/var`, `/etc`, or the jailbreak-managed `/var/jb`. It packages the
files and child directories beneath the existing installation prefix. The
persistent runtime log directory lives in writable user-data storage; the
package does not alter ownership of the existing `/var/mobile` parent folders.

## Configure multiple paths and exclusions

The package includes the appropriate `fsmod.conf` example. Its report URL is
`http://119.8.59.179:8085/events`, the OpenResty endpoint supplied for this
deployment. Review the monitored and excluded paths before enabling reporting.

Values are plain, unquoted `key=value` lines. Repeat `watch_path` and
`exclude_path`; do not put several paths in one comma-separated value. Example:

```ini
report_url=http://119.8.59.179:8085/events
# report_token=your-bearer-token
backend=devfsev

watch_path=/private/var/mobile
watch_path=/private/var/containers/Bundle/Application

exclude_path=/private/var/mobile/Library/Caches
exclude_path=/private/var/mobile/Library/Logs
exclude_path=/private/var/mobile/Media/DCIM
# A single file can also be excluded, once this file exists:
# exclude_path=/private/var/mobile/Documents/ignore.txt

# Optional process exclusions; uncomment only the names you intend to omit:
# exclude_process=zsh
# exclude_process=sshd
# exclude_process=SpringBoard

# Optional global event-UID rules; each value is separate, not comma-separated:
# watch_uid=0
# watch_uid=501
# exclude_uid=501

# Optional direct parent process PID rules; example PIDs only:
# watch_ppid=1
# watch_ppid=1234
# exclude_ppid=1234

daemonize=false
pid_file=/var/run/fsmod.pid
log_file=/var/mobile/Library/Logs/fsmod/fsmod.log
log_events=false
queue_capacity=1024
report_timeout_seconds=10
report_retries=3
retry_delay_seconds=2
shutdown_timeout_seconds=15
```

- Configure at least one absolute `watch_path`; at most 64 entries are allowed,
  counting missing paths too. A watch path that cannot be resolved with `ENOENT`
  or `ENOTDIR` produces a warning and is skipped. Other path errors remain fatal.
  Existing watch roots are resolved using `realpath`, including symlinks.
- If all watch paths are skipped, startup succeeds with `roots=0` and the service
  stays idle. `-T` also returns success with warnings if the remaining configuration
  is valid. Creating a skipped path later does not add it automatically; restart
  fsmod to watch it.
- Every `exclude_path` must still exist on the target device at startup; missing
  exclusions remain errors.
- A directory rule covers that directory and its descendants; a file rule covers
  that file. Rules match whole path components. The most specific matching
  `watch_path` or `exclude_path` wins, regardless of configuration order. A deeper
  watch restores monitoring within a parent exclusion; a deeper exclusion can
  exclude part of that watch again. At the same path, exclusion wins.
- Startup and `-T` reject the configuration when every effective watch is still
  excluded. A deeper watch restored inside a parent exclusion counts as monitored scope.
- Nested watch paths are allowed; duplicate canonical watch paths are rejected.
  Backend collection uses the outermost nonoverlapping roots to avoid duplicate
  collection, while all logical watch rules remain active.
- Use `/private/var/...` consistently on iOS. Rootless installation under
  `/var/jb` does not move user data under `/var/jb/private/var`.
- The PID and log files are automatically suppressed to prevent self-reporting.
  Excluding the whole log directory also covers future log files.
- Keep the config owned by root and mode `0600`, especially if it contains a token.

For example, monitor `/Library` and `/Library/dpkg/info`, while excluding the
rest of `/Library/dpkg`:

```ini
watch_path=/Library
watch_path=/Library/dpkg/info
exclude_path=/Library/dpkg
```

Optional `watch_events` belongs to the most recent `watch_path`, even across
comments and unrelated settings. Use at most one per path. Every new path
defaults to `all`, including nested watches. The most specific matching watch
supplies the event selection; it does not inherit or combine parent selections.
Commented examples:

```ini
# watch_path=/private/etc
# watch_events=create,modify
# watch_path=/bin
# watch_events=all
# watch_path=/sbin
# watch_events=create,FSE_CONTENT_MODIFIED
```

The case-sensitive groups are `create` (`FSE_CREATE_FILE`, `FSE_CREATE_DIR`),
`modify` (`FSE_CONTENT_MODIFIED`, `FSE_STAT_CHANGED`, `FSE_CHOWN`,
`FSE_XATTR_MODIFIED`, `FSE_XATTR_REMOVED`, `FSE_FINDER_INFO_CHANGED`),
`delete` (`FSE_DELETE`), `rename` (`FSE_RENAME`), `clone` (`FSE_CLONE`), and
`exchange` (`FSE_EXCHANGE`). Comma-separated groups can be mixed with exact
event names. Exact names also include `FSE_DOCID_CREATED`, `FSE_DOCID_CHANGED`,
`FSE_UNMOUNT_PENDING`, `FSE_OPEN`, `FSE_CLOSE`, `FSE_CLOSE_WRITABLE`, and
`FSE_UNKNOWN`. `all` must appear alone and is the default.

Unknown names, empty items (including trailing commas), mixed `all`, a rule
without a preceding `watch_path`, and a second rule for the same path are
configuration errors; repeated tokens are harmless. A rule for a missing path
is still validated, but cannot alter the preceding effective root's rule.
For rename and clone events, each path is resolved independently using the most
specific rule. A final exclusion on either side omits the whole event; a path
outside all rules is not excluded. Otherwise, at least one included path's most
specific watch must allow the event type.

These settings filter HTTP reporting and `log_events`, not kernel subscriptions.
`all` only accepts events already provided by the backend. It does not add read
tracking or identify the source of a libc `fread`/`fwrite` copy. Restart after edits.

`exclude_process` may be repeated for multiple exact, case-sensitive names from
`data.proc`; duplicate entries are deduplicated. No process exclusions are
enabled by default. For example, `exclude_process=zsh` suppresses events whose
reported process name is exactly `zsh`; it does not suppress children launched
by that shell. PIDs, executable paths, wildcards and substring matching are not
supported. The kernel still collects events, but fsmod discards a matching
event before both HTTP queueing and `log_events` output. Restart the daemon
after changing these rules.

The iOS `devfsev` backend normally supplies the process name. If a process exits
before its name can be resolved, or metadata is otherwise unavailable, the event
remains eligible under the other filters. Names may also be truncated by the OS;
use the actual value in `data.proc`. The setting is accepted with every backend:
`fsevapi` and `inotify` often have no originating-process name, while Linux
`fanotify` provides it when available.

Optional `watch_uid` and `exclude_uid` filter the backend-provided `data.uid`.
With `devfsev`, this is the file owner's UID from `FSE_ARG_UID`/file-info
metadata, not a guarantee of the UID of the process performing the operation.
For example, an operation performed by root on a file owned by mobile can carry
UID `501`. fsmod does not replace missing metadata with its own `getuid()`, a
later `stat()` result or inferred process credentials. Current other backends
do not supply a usable event UID; a zero in the JSON alone can also mean that
metadata was unavailable.

Each rule takes one decimal UID from `0` to `4294967295`. Repeat either key for
multiple UIDs; duplicate values are deduplicated, with no fixed count limit
apart from available memory. User names, PIDs, device UUIDs, comma-separated
values, signs and wildcards are rejected. The examples remain commented out:

```ini
# Accept only known UID 0 or 501:
# watch_uid=0
# watch_uid=501
# If also enabled, this excludes 501 and leaves only 0:
# exclude_uid=501
```

Without either key, UID does not restrict events. With `watch_uid`, only listed
UIDs pass; with only `exclude_uid`, every known UID except those listed is
eligible. An exclusion takes precedence when a UID is in both lists. Once any
UID rule is configured, events whose UID is unavailable are discarded.
UID rules are global and combine with path, event-type and process rules; an
event must pass all filters. They apply before both HTTP queueing and
`log_events`, while kernel collection continues. Restart after changes. The
reported JSON schema remains unchanged.

Optional `watch_gid` and `exclude_gid` filter the backend-provided `data.gid`.
With `devfsev`, this is the file owner's group GID from event metadata, not the
GID of the process performing the operation or its supplementary groups. Current
other backends do not provide a usable event GID.

Each rule takes one decimal GID from `0` to `4294967295`. Repeat either key for
multiple GIDs; duplicate values are deduplicated, with no fixed count limit
apart from available memory. With `watch_gid`, only listed GIDs pass; with only
`exclude_gid`, every known GID except those listed is eligible. An exclusion
takes precedence when a value occurs in both lists. Once any GID rule is
configured, events whose GID is unavailable are discarded.

UID and GID rule groups combine with AND semantics, along with path, event-type
and process rules. They are not paired by line and do not form an OR across
groups. GID rules apply before HTTP queueing and `log_events`; kernel collection
continues. Restart after changes. The reported JSON schema remains unchanged.

Optional `watch_ppid` and `exclude_ppid` filter `data.ppid`, the operation
process's direct parent PID resolved when fsmod consumes the event. Each line
accepts one decimal value from `1` to `2147483647`; signs, names, comma-separated
lists and wildcards are invalid. Repeat either key for multiple values;
duplicates are deduplicated, with no fixed count limit apart from memory.
Without either key, PPID does not restrict events. A nonempty `watch_ppid`
list accepts only listed parents, and `exclude_ppid` takes precedence. With
any PPID rule, unavailable or nonpositive PPIDs are discarded.

Apple `devfsev` and Linux `fanotify` attempt to resolve PPID; `fsevapi` and
`inotify` do not provide it. This is not a parent process name, an ancestor,
a file UID/GID, or a stable process identity; process exit and PID reuse can
affect the result. `watch_ppid=1` selects events with a resolved direct parent
PID of 1, not all descendants of PID 1.

PPID rules combine with UID, GID, path, event-type and process rules using AND.
Every filter must pass before HTTP queueing and `log_events`; kernel collection
continues. Restart after changes. The reported JSON schema remains unchanged.

## Validate and start on the device

Install only the package matching the device's jailbreak layout. The installer
validates the default configuration and starts the launchd service automatically.
For Rootful, the program reads `/etc/fsmod.conf`; for Rootless it reads the
`etc/fsmod.conf` beside the actual jailbreak installation prefix, normally
`/var/jb/etc/fsmod.conf`. It does not choose between two configurations based on
which one happens to exist. The configuration remains editable on disk.

No startup arguments are required. As root, use these commands for validation or
manual foreground operation after stopping the launchd job:

```sh
# Rootful
/usr/sbin/fsmod -T
/usr/sbin/fsmod

# Rootless
/var/jb/usr/sbin/fsmod -T
/var/jb/usr/sbin/fsmod
```

Launchd `ProgramArguments` contains only the matching `usr/sbin/fsmod` path.
Rootful installs the plist at
`/Library/LaunchDaemons/com.apple.fsmod.plist`.
Rootless keeps `/var/jb/Library/LaunchDaemons/com.apple.fsmod.plist`,
the jailbreak launchd discovery path.
The package does not install a `usr/bin/fsmod` executable or compatibility link.
The process stays in the foreground, even if the saved configuration contains
`daemonize=true`; launchd supervises it. An explicit `-C` override remains
available, and `-T` / `-S` use the same default configuration when `-C` is omitted.

`-T` validates configuration and paths, but does not prove that `/dev/fsevents`
can be opened or that the server is reachable. A foreground smoke test makes
backend startup failures observable; HTTP results still depend on real delivery.
If the service has been manually disabled, re-enable the Rootful job with:

```sh
launchctl enable system/com.apple.fsmod
launchctl bootstrap system /Library/LaunchDaemons/com.apple.fsmod.plist
```

For Rootless, substitute
`/var/jb/Library/LaunchDaemons/com.apple.fsmod.plist`. If launchctl does not
support `enable`/`bootstrap`, use the older form with the matching plist path:

```sh
launchctl load -w /Library/LaunchDaemons/com.apple.fsmod.plist
```

Do not start a foreground instance while launchd already owns a running instance.
The PID lock prevents concurrent instances. For a config change, validate again,
then restart the existing job:

```sh
launchctl kickstart -k system/com.apple.fsmod
```

If the jailbreak only supports the older interface, unload then load the plist.
To stop and disable future automatic starts:

```sh
launchctl bootout system/com.apple.fsmod
launchctl disable system/com.apple.fsmod
```

The older equivalent is `launchctl unload -w <matching-plist-path>`. Running
`fsmod -S` alone stops the current process; launchd's
`KeepAlive` can start it again. Logs are in the path listed in the table above.

## Control a running daemon during other package updates

The installed executable also controls the running instance. These commands
require root on the device and leave the launchd job running:

```sh
# Rootful
/usr/sbin/fsmod ctl status --json
/usr/sbin/fsmod ctl pause --id com.example.package --scope all --ttl 600
# Update your package's files only after pause exits successfully.
/usr/sbin/fsmod ctl resume --id com.example.package

# Rootless
/var/jb/usr/sbin/fsmod ctl status --json
/var/jb/usr/sbin/fsmod ctl pause --id com.example.package --scope all --ttl 600
/var/jb/usr/sbin/fsmod ctl resume --id com.example.package

# Cover an entire installation, including work before maintainer scripts run.
/usr/sbin/fsmod ctl run --scope all --ttl 600 -- dpkg -i yourpackage.deb
/var/jb/usr/sbin/fsmod ctl run --scope all --ttl 600 -- dpkg -i yourpackage.deb
```

Use a stable, distinct package identifier for `--id`. It must contain 1 to 127
ASCII letters, digits, dots, underscores or hyphens. A maximum of 64 IDs may
hold pauses simultaneously. Repeating `pause` with the same ID renews the hold
and replaces its scope. `resume` releases only that ID; an absent ID succeeds
without changing any other package's hold. This lets overlapping package
updates complete independently. The TTL accepts 1 through 86400 whole seconds,
defaults to 600, and expires automatically. Renew before it expires for a
longer update. The default scope is `all`.

| Scope | Event handling | Local event log | HTTP |
| --- | --- | --- | --- |
| `all` | Consume stream and discard | Suppressed | Suppressed |
| `report` | Normal processing | Follows `log_events` | Suppressed |

An `all` hold takes precedence over a `report` hold. The kernel backend stays
open and consumes events; the process is not suspended or restarted. Events
handled during the hold do not enter the corresponding output queue and are
not replayed when it ends. On `devfsev`, kernel timestamps also discard delayed
events from before the resume boundary. Other backends without those timestamps
apply the hold when their callbacks are processed; a delayed callback may still
appear after resume.

Reports queued before pausing remain eligible after all reporting holds end.
Add `--discard-pending` when those older reports should also be removed:

```sh
/usr/sbin/fsmod ctl pause --id com.example.package --scope all --ttl 600 --discard-pending
# Only suppress HTTP, retaining event logs if log_events=true:
/usr/sbin/fsmod ctl pause --id com.example.package --scope report --ttl 600
```

A successful pause gates event handling and waits until any active HTTP transfer
has finished or cancelled. Check the exit status before making changes. A
nonzero exit leaves the operation unconfirmed; an HTTP cancellation timeout can
leave the hold active, so inspect `status` before retrying. Already received HTTP
requests cannot be withdrawn, and interruption does not prove the server did
not store one. Retained event queues remain memory-only and are not sent during
a paused shutdown.

For a package you maintain, place `pause` before its file changes and `resume`
after maintenance completes, with the same ID in both scripts. Also attempt
release on the package's failure/abort paths; TTL handles missed cleanup. A
maintainer script runs only when invoked by the package manager, so it cannot
cover filesystem changes that precede its invocation. For that whole-operation
boundary, invoke `fsmod ctl run --scope all --ttl 600 -- dpkg -i yourpackage.deb`
from outside the package instead.

`ctl run` creates a unique hold, waits for pause acknowledgement, then starts
the command. When the command exits, it releases only that hold; other package
holds remain active. It accepts `--scope all|report`, `--ttl SECONDS`,
`--discard-pending` and `--socket PATH` before the mandatory `--` separator.
Defaults are `all` and 600 seconds. `--id` and `--json` are not accepted. A
nonempty command is required after `--`; all later arguments belong to that
command, including a literal `--socket`. Arguments are executed directly, so
use an explicit shell if shell syntax is required.

The command inherits standard input/output/error; wrapper diagnostics go to
standard error and no control JSON is added to standard output. If pause fails
or the daemon is unavailable, the command is not started. Interactive input is
supported for foreground commands, but shell job control (`Ctrl-Z`, `fg`, `bg`)
is not supported; avoid suspending the command during a maintenance hold.
Cleanup is attempted after child exit, launch failure, and catchable
`SIGINT`, `SIGTERM` or `SIGHUP`;
the wrapper forwards those signals to the child process group. It returns the
child exit code or `128 + signal` (127 for a missing executable, 126 for other
launch failures). A cleanup error changes a successful child result to nonzero
but preserves an already nonzero result. Detached descendants or processes
that outlive the direct command are outside this lifetime.

**`run` does not renew its TTL.** Choose a value longer than the expected update,
from 1 through 86400 seconds. When the TTL expires, fsmod resumes even if the
command is still running. `SIGKILL` prevents wrapper cleanup, so expiration is
the fallback. For an update that may take longer, select a larger TTL before
starting, for example:

```sh
/var/jb/usr/sbin/fsmod ctl run --scope all --ttl 1800 -- dpkg -i yourpackage.deb
```

Control uses a mode-0600 Unix-domain socket and checks the peer UID. The socket
is normally the configured `pid_file` plus `.ctl`; with the packaged configuration both
layouts use `/var/run/fsmod.pid.ctl`. The private state file is
`/var/run/fsmod.pid.ctl.state`. Unexpired holds restore before event handling
after a daemon restart on the same boot; reboot invalidates the prior boot's
holds. Preserve the state file while maintenance is active. It stores holds,
not event data or counters. For unusually long PID paths, the socket falls back
to a deterministic `/tmp/fsmod-<uid>-<path-hash>.ctl`; the state file remains
`pid_file` plus `.ctl.state`.

Without `--socket`, control loads the default configuration or an explicit
`-C` configuration to find the PID path. `--socket` bypasses configuration
loading, useful if the configuration changed after startup:

```sh
/usr/sbin/fsmod ctl status --json --socket /var/run/fsmod.pid.ctl
/usr/sbin/fsmod -C /path/to/custom.conf ctl status --json
```

Responses from `status`, `pause` and `resume` are JSON. `status --json` is an
explicit script-friendly equivalent to `status`. Successful responses contain
`ok`, `pid`, `monitor_paused`, `report_paused`, `queued`, `in_flight`,
`delivered`, `failed`, `dropped`, `suppressed_monitor`,
`suppressed_report`, and `holds`. Each hold contains `id`, `scope` and
`remaining_seconds`. Counters cover the current daemon process. Static watch,
exclusion and destination settings still require a restart after edits; these
commands do not reload them. Service lifecycle/control log messages remain
enabled even while file-event logs are suppressed.
