#!/usr/bin/env python3
"""Package an already-built, signed iOS fsmod binary without dpkg-deb."""

import argparse
import gzip
import hashlib
import io
import os
from pathlib import Path
import re
import struct
import sys
import tarfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PACKAGE_ID = "com.apple.fsmod"
SERVICE_LABEL = "com.apple.fsmod"
LEGACY_PACKAGE_ID = "com.nowsecure.fsmod"
VARIANTS = {
    "rootful": ("", "iphoneos-arm"),
    "rootless": ("var/jb/", "iphoneos-arm64"),
}


def package_version():
    for line in (ROOT / "config.mk").read_text(encoding="utf-8").splitlines():
        if line.startswith("VERSION="):
            return line.partition("=")[2].strip()
    raise ValueError("VERSION is missing from config.mk")


def regular_file(path):
    path = Path(path).resolve()
    if not path.is_file():
        raise argparse.ArgumentTypeError("binary must name an existing regular file")
    return path


def tar_gzip(entries, epoch):
    """Create reproducible archives without relying on the host uid or tar."""
    tar_bytes = io.BytesIO()
    with tarfile.open(fileobj=tar_bytes, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for name, contents, mode in sorted(entries, key=lambda item: item[0]):
            info = tarfile.TarInfo("./" + name)
            info.mtime = epoch
            info.uid = info.gid = 0
            info.uname, info.gname = "root", "wheel"
            info.mode = mode
            if contents is None:
                info.type = tarfile.DIRTYPE
                archive.addfile(info)
            else:
                info.size = len(contents)
                archive.addfile(info, io.BytesIO(contents))
    compressed = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=compressed, mtime=epoch) as output:
        output.write(tar_bytes.getvalue())
    return compressed.getvalue()


def ar_archive(members, epoch):
    """Debian uses these three ar members, in precisely this order."""
    output = bytearray(b"!<arch>\n")
    for name, contents in members:
        header = ("{:<16}{:<12}{:<6}{:<6}{:<8}{:<10}`\n".format(
            name + "/", epoch, 0, 0, "100644", len(contents)
        )).encode("ascii")
        if len(header) != 60:
            raise ValueError("ar header exceeds the supported field width")
        output.extend(header)
        output.extend(contents)
        if len(contents) % 2:
            output.extend(b"\n")
    return bytes(output)


def make_package(binary, variant, args):
    prefix, architecture = VARIANTS[variant]
    shell = "/" + prefix + "bin/sh"
    launchctl = "/" + prefix + "bin/launchctl"
    daemon_directory = prefix + "Library/LaunchDaemons/"
    # A rootful package briefly shipped the job in System/Library. Retire that
    # exact file when restoring the original directory to avoid duplicate jobs.
    old_plist = (("System/Library/LaunchDaemons/" if variant == "rootful"
                  else daemon_directory) + SERVICE_LABEL + ".plist")
    legacy_plist = prefix + "Library/LaunchDaemons/" + LEGACY_PACKAGE_ID + ".plist"
    old_plist_unload = (
        "    {launchctl} unload /{old_plist} >/dev/null 2>&1 || true\n".format(
            launchctl=launchctl, old_plist=old_plist)
        if old_plist != daemon_directory + SERVICE_LABEL + ".plist" else ""
    )
    old_plist_cleanup = (
        "    rm -f /{old_plist}\n".format(old_plist=old_plist)
        if old_plist != daemon_directory + SERVICE_LABEL + ".plist" else ""
    )
    paths = {
        "binary": prefix + "usr/sbin/fsmod",
        "config": prefix + "etc/fsmod.conf",
        "plist": daemon_directory + SERVICE_LABEL + ".plist",
        "man": prefix + "usr/share/man/man1/fsmod.1",
    }
    files = [
        (paths["binary"], binary, 0o755),
        (paths["config"], (HERE / ("fsmod." + variant + ".conf.example")).read_bytes(), 0o600),
        (paths["plist"], (HERE / (SERVICE_LABEL + "." + variant + ".plist")).read_bytes(), 0o644),
        (paths["man"], (ROOT / "fsmod.1").read_bytes(), 0o644),
    ]
    directories = set()
    for entry in files:
        name = entry[0]
        parent = Path(name).parent
        while str(parent) != ".":
            directories.add(parent.as_posix())
            parent = parent.parent
    # /var and /etc are system symlinks; /var/jb is a jailbreak-managed symlink.
    # Never package those anchors as directories or claim their ownership.
    # The bootstrap already supplies these parent directories on the device.
    # Do not change ownership or permissions of the existing System tree.
    shared_anchors = {"var", "etc", "usr", "Library", "var/jb",
                      "System", "System/Library", "System/Library/LaunchDaemons"}
    data_entries = [(name, None, 0o755) for name in directories - shared_anchors] + files
    # Do not ship ownership or permissions for the existing mobile user folders.
    data_entries.append(("var/mobile/Library/Logs/fsmod", None, 0o750))
    installed_size = (sum(len(contents) for _, contents, _ in files) + 1023) // 1024
    replacement = (
        "Conflicts: {legacy}\nReplaces: {legacy}\n".format(legacy=LEGACY_PACKAGE_ID)
        if args.package_id != LEGACY_PACKAGE_ID else ""
    )
    control = (
        "Package: {package}\n"
        "Version: {version}\n"
        "Architecture: {architecture}\n"
        "Maintainer: fsmod maintainers\n"
        "Section: Utilities\n"
        "Priority: optional\n"
        "{replacement}"
        "Installed-Size: {size}\n"
        "Description: iOS filesystem event reporting daemon ({variant})\n"
        " Reports filesystem events using the private devfsev backend.\n"
        " Reads the default configuration and starts automatically with launchd.\n"
    ).format(package=args.package_id, version=args.version, architecture=architecture,
             size=installed_size, variant=variant, replacement=replacement).encode("utf-8")
    sums = "".join(
        hashlib.md5(contents).hexdigest() + "  " + name + "\n"
        for name, contents, _ in sorted(files)
    ).encode("ascii")
    # Removal stops the job; preinst/postinst reload it during an upgrade.
    prerm = (
        "#!{shell}\n"
        "case \"${{1:-}}\" in\n"
        "  remove|deconfigure)\n"
        "    {launchctl} bootout system/{label} >/dev/null 2>&1 || \\\n"
        "      {launchctl} unload /{plist} >/dev/null 2>&1 || true\n"
        "{old_plist_unload}"
        "    ;;\n"
        "esac\n"
        "exit 0\n"
    ).format(shell=shell, launchctl=launchctl, label=SERVICE_LABEL,
             plist=paths["plist"], old_plist_unload=old_plist_unload).encode("ascii")
    # Unload both labels before replacing the old executable path. A kickstart
    # alone would keep the existing job's old ProgramArguments.
    preinst = (
        "#!{shell}\n"
        "case \"${{1:-}}\" in\n"
        "  install|upgrade|abort-upgrade)\n"
        "    {launchctl} bootout system/{legacy} >/dev/null 2>&1 || \\\n"
        "      {launchctl} unload /{legacy_plist} >/dev/null 2>&1 || true\n"
        "    {launchctl} bootout system/{label} >/dev/null 2>&1 || \\\n"
        "      {launchctl} unload /{plist} >/dev/null 2>&1 || true\n"
        "{old_plist_unload}"
        "    ;;\n"
        "esac\n"
        "exit 0\n"
    ).format(shell=shell, launchctl=launchctl, legacy=LEGACY_PACKAGE_ID, label=SERVICE_LABEL,
             plist=paths["plist"], old_plist_unload=old_plist_unload,
             legacy_plist=legacy_plist).encode("ascii")
    postinst = (
        "#!{shell}\n"
        "set -e\n"
        "case \"${{1:-}}\" in\n"
        "  configure)\n"
        "    if ! /{binary} -T; then\n"
        "      echo 'fsmod: configuration validation failed; edit fsmod.conf and reconfigure the package' >&2\n"
        "      exit 1\n"
        "    fi\n"
        "    {launchctl} bootout system/{label} >/dev/null 2>&1 || \\\n"
        "      {launchctl} unload /{plist} >/dev/null 2>&1 || true\n"
        "{old_plist_unload}"
        "{old_plist_cleanup}"
        "    {launchctl} enable system/{label} >/dev/null 2>&1 || true\n"
        "    if ! {launchctl} bootstrap system /{plist}; then\n"
        "      if ! {launchctl} load -w /{plist}; then\n"
        "        echo 'fsmod: launchd activation failed; inspect the service log' >&2\n"
        "        exit 1\n"
        "      fi\n"
        "    fi\n"
        "    ;;\n"
        "esac\n"
        "exit 0\n"
    ).format(shell=shell, binary=paths["binary"], launchctl=launchctl,
             label=SERVICE_LABEL, plist=paths["plist"],
             old_plist_unload=old_plist_unload,
             old_plist_cleanup=old_plist_cleanup).encode("ascii")
    control_entries = [
        ("control", control, 0o644),
        ("conffiles", ("/" + paths["config"] + "\n").encode("ascii"), 0o644),
        ("md5sums", sums, 0o644),
        ("prerm", prerm, 0o755),
        ("preinst", preinst, 0o755),
        ("postinst", postinst, 0o755),
    ]
    contents = ar_archive([
        ("debian-binary", b"2.0\n"),
        ("control.tar.gz", tar_gzip(control_entries, args.source_date_epoch)),
        ("data.tar.gz", tar_gzip(data_entries, args.source_date_epoch)),
    ], args.source_date_epoch)
    destination = args.output_dir / (args.package_id + "_" + args.version + "_" + architecture + ".deb")
    destination.write_bytes(contents)
    print(str(destination.resolve()))
    print("  sha256 " + hashlib.sha256(contents).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=regular_file,
                        help="already-built, signed iOS arm64 Mach-O executable")
    parser.add_argument("--variant", choices=["rootful", "rootless", "both"], default="both")
    parser.add_argument("--version", default=package_version())
    parser.add_argument("--package-id", default=PACKAGE_ID)
    parser.add_argument("--output-dir", type=Path, default=ROOT / "out" / "ios")
    parser.add_argument("--source-date-epoch", type=int, default=int(os.environ.get("SOURCE_DATE_EPOCH", "0")))
    args = parser.parse_args()
    if not re.fullmatch(r"[a-z0-9][a-z0-9+.-]+", args.package_id):
        parser.error("--package-id must be a valid Debian package name")
    if not re.fullmatch(r"[0-9][A-Za-z0-9.+:~\-]*", args.version):
        parser.error("--version must be a valid Debian package version")
    if not 0 <= args.source_date_epoch <= 0xffffffff:
        parser.error("--source-date-epoch must be between 0 and 4294967295")
    binary = args.binary.read_bytes()
    if len(binary) < 32 or binary[:4] != b"\xcf\xfa\xed\xfe" or struct.unpack_from("<I", binary, 4)[0] != 0x0100000c:
        parser.error("--binary must be a thin arm64 Mach-O executable; use the iOS build output")
    if struct.unpack_from("<I", binary, 12)[0] != 2:
        parser.error("--binary must have Mach-O file type MH_EXECUTE")
    command_count, command_bytes = struct.unpack_from("<II", binary, 16)
    position, commands_end = 32, 32 + command_bytes
    if commands_end > len(binary):
        parser.error("truncated Mach-O load commands")
    ios_platform, signed = False, False
    for _ in range(command_count):
        if position + 8 > commands_end:
            parser.error("invalid Mach-O load command count")
        command, size = struct.unpack_from("<II", binary, position)
        if size < 8 or position + size > commands_end:
            parser.error("invalid Mach-O load command size")
        if command == 0x32 and size >= 24:  # LC_BUILD_VERSION
            if struct.unpack_from("<I", binary, position + 8)[0] != 2:
                parser.error("Mach-O platform is not a physical iOS device")
            ios_platform = True
        elif command == 0x25 and size >= 16:  # LC_VERSION_MIN_IPHONEOS
            ios_platform = True
        elif command == 0x1d and size >= 16:  # LC_CODE_SIGNATURE
            offset, length = struct.unpack_from("<II", binary, position + 8)
            signed = length > 0 and offset + length <= len(binary)
        position += size
    if position != commands_end or not ios_platform:
        parser.error("--binary must target physical iOS, not macOS or Simulator")
    if not signed:
        parser.error("--binary must be signed with ldid before packaging")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for variant in VARIANTS if args.variant == "both" else [args.variant]:
        make_package(binary, variant, args)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, tarfile.TarError) as error:
        sys.exit("package error: " + str(error))
