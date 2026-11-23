#!/usr/bin/env python3
"""Check iOS package contents and the legacy launchd migration without dpkg."""

import contextlib
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[1]
PACKAGER = ROOT / "dist" / "ios" / "package.py"
SPEC = importlib.util.spec_from_file_location("fsmod_ios_package", PACKAGER)
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)


def fixture_binary(platform=2, signed=True):
    """Minimal structural fixture for the CLI's Mach-O checks, not executable."""
    commands = struct.pack("<6I", 0x32, 24, platform, 0x000C0000, 0x00110000, 0)
    if signed:
        commands += struct.pack("<4I", 0x1D, 16, 32 + 24 + 16, 8)
    header = struct.pack("<8I", 0xFEEDFACF, 0x0100000C, 0, 2,
                         2 if signed else 1, len(commands), 0, 0)
    return header + commands + (b"signature"[:8] if signed else b"")


def unpack_deb(contents):
    if contents[:8] != b"!<arch>\n":
        raise AssertionError("not a Debian ar archive")
    result = {}
    position = 8
    while position < len(contents):
        header = contents[position:position + 60]
        if len(header) != 60 or header[58:] != b"`\n":
            raise AssertionError("invalid ar header")
        name = header[:16].decode("ascii").strip().rstrip("/")
        length = int(header[48:58])
        position += 60
        result[name] = contents[position:position + length]
        if len(result[name]) != length:
            raise AssertionError("truncated ar member")
        position += length + length % 2
    if position != len(contents):
        raise AssertionError("invalid ar padding")
    if list(result) != ["debian-binary", "control.tar.gz", "data.tar.gz"]:
        raise AssertionError("unexpected Debian member order")
    if result["debian-binary"] != b"2.0\n":
        raise AssertionError("invalid Debian format version")
    return result


def unpack_tar(contents):
    result = {}
    with tarfile.open(fileobj=io.BytesIO(contents), mode="r:gz") as archive:
        for info in archive:
            name = info.name.removeprefix("./")
            stream = archive.extractfile(info) if info.isfile() else None
            result[name] = (info, stream.read() if stream else None)
    return result


def control_fields(contents):
    return dict(line.split(": ", 1) for line in contents.decode().splitlines()
                if line and not line.startswith(" "))


class IosPackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="fsmod-package-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.binary = fixture_binary()

    def build(self, variant, package_id="com.apple.fsmod"):
        args = SimpleNamespace(package_id=package_id, version="1.0.0",
                               source_date_epoch=1234567890, output_dir=self.directory)
        with contextlib.redirect_stdout(io.StringIO()):
            package.make_package(self.binary, variant, args)
        architecture = "iphoneos-arm" if variant == "rootful" else "iphoneos-arm64"
        return self.directory / (package_id + "_1.0.0_" + architecture + ".deb")

    def run_cli(self, binary, *extra):
        binary_path = self.directory / "fsmod-fixture"
        binary_path.write_bytes(binary)
        return subprocess.run([sys.executable, str(PACKAGER), "--binary", str(binary_path),
                               "--output-dir", str(self.directory),
                               "--source-date-epoch", "0", *extra],
                              cwd=ROOT, capture_output=True, text=True, check=False)

    def test_rootful_and_rootless_contents(self):
        for variant, prefix, architecture in [("rootful", "", "iphoneos-arm"),
                                                ("rootless", "var/jb/", "iphoneos-arm64")]:
            with self.subTest(variant=variant):
                members = unpack_deb(self.build(variant).read_bytes())
                control = unpack_tar(members["control.tar.gz"])
                data = unpack_tar(members["data.tar.gz"])
                fields = control_fields(control["control"][1])
                self.assertEqual(fields["Package"], "com.apple.fsmod")
                self.assertEqual(fields["Version"], "1.0.0")
                self.assertEqual(fields["Architecture"], architecture)
                self.assertEqual(fields["Conflicts"], "com.nowsecure.fsmod")
                self.assertEqual(fields["Replaces"], "com.nowsecure.fsmod")
                self.assertEqual(control["conffiles"][1],
                                 ("/" + prefix + "etc/fsmod.conf\n").encode())
                binary_name = prefix + "usr/sbin/fsmod"
                self.assertEqual(data[binary_name][1], self.binary)
                self.assertTrue(data[binary_name][0].isfile())
                self.assertEqual(data[binary_name][0].mode, 0o755)
                for obsolete_binary in ["usr/bin/fsmod", "var/jb/usr/bin/fsmod"]:
                    self.assertNotIn(obsolete_binary, data.keys())
                self.assertFalse(any(info.issym() for info, _ in data.values()))
                self.assertEqual([name for name, (info, _) in data.items()
                                  if info.isfile() and Path(name).name == "fsmod"],
                                 [binary_name])
                self.assertEqual(data[prefix + "etc/fsmod.conf"][0].mode, 0o600)
                self.assertIn(prefix + "usr/share/man/man1/fsmod.1", data)
                self.assertFalse(any("com.nowsecure.fsmod" in path for path in data))
                old_plist = "System/Library/LaunchDaemons/com.apple.fsmod.plist"
                self.assertNotIn(old_plist, data.keys())
                plist_name = prefix + "Library/LaunchDaemons/com.apple.fsmod.plist"
                plist = plistlib.loads(data[plist_name][1])
                self.assertEqual(plist["Label"], "com.apple.fsmod")
                self.assertEqual(plist["ProgramArguments"],
                                 ["/" + prefix + "usr/sbin/fsmod"])
                self.assertFalse(plist.get("Disabled", False))
                self.assertTrue(plist["RunAtLoad"])
                self.assertTrue(plist["KeepAlive"])
                self.assertEqual(data[plist_name][0].mode, 0o644)
                for name in ["var", "etc", "usr", "Library", "var/jb", "System",
                             "System/Library", "System/Library/LaunchDaemons"]:
                    self.assertNotIn(name, data)
                self.assertFalse(any(path.startswith("System/") or
                                     path.startswith("var/jb/System/") for path in data))
                for info, _ in data.values():
                    self.assertEqual((info.uid, info.gid), (0, 0))

                checksummed_paths = set()
                for line in control["md5sums"][1].decode().splitlines():
                    digest, path = line.split("  ", 1)
                    checksummed_paths.add(path)
                    self.assertTrue(data[path][0].isfile())
                    self.assertEqual(digest, hashlib.md5(data[path][1]).hexdigest())
                self.assertEqual(checksummed_paths,
                                 {path for path, (info, _) in data.items() if info.isfile()})
                self.assertNotIn("usr/bin/fsmod", checksummed_paths)
                self.assertNotIn("var/jb/usr/bin/fsmod", checksummed_paths)

                preinst = control["preinst"][1].decode()
                prerm = control["prerm"][1].decode()
                postinst = control["postinst"][1].decode()
                launchctl = "/" + prefix + "bin/launchctl"
                self.assertTrue(preinst.startswith("#!/" + prefix + "bin/sh\n"))
                self.assertIn("install|upgrade|abort-upgrade)", preinst)
                self.assertIn(launchctl + " bootout system/com.nowsecure.fsmod", preinst)
                self.assertIn(launchctl + " unload /" + prefix +
                              "Library/LaunchDaemons/com.nowsecure.fsmod.plist", preinst)
                self.assertIn(launchctl + " bootout system/com.apple.fsmod", preinst)
                self.assertIn(launchctl + " unload /" + plist_name, preinst)
                self.assertIn("remove|deconfigure)", prerm)
                self.assertIn(launchctl + " bootout system/com.apple.fsmod", prerm)
                self.assertIn(launchctl + " unload /" + plist_name, prerm)
                if variant == "rootful":
                    for script in [preinst, prerm, postinst]:
                        self.assertIn(launchctl + " unload /" + old_plist, script)
                self.assertNotIn("upgrade)", prerm)
                self.assertTrue(postinst.startswith("#!/" + prefix + "bin/sh\n"))
                self.assertIn("configure)", postinst)
                config_check = "/" + binary_name + " -T"
                self.assertIn(config_check, postinst)
                self.assertIn(launchctl + " enable system/com.apple.fsmod", postinst)
                self.assertIn(launchctl + " bootstrap system /" + plist_name, postinst)
                self.assertIn(launchctl + " load -w /" + plist_name, postinst)
                self.assertIn(launchctl + " bootout system/com.apple.fsmod", postinst)
                self.assertLess(postinst.index(config_check),
                                postinst.index(launchctl + " bootout"))
                self.assertLess(postinst.index(config_check),
                                postinst.index(launchctl + " enable"))
                self.assertLess(postinst.index(config_check),
                                postinst.index(launchctl + " bootstrap"))
                self.assertIn("exit 1", postinst)
                for name in ["preinst", "prerm", "postinst"]:
                    self.assertEqual(control[name][0].mode, 0o755)
                    script = control[name][1].decode()
                    if name != "postinst":
                        for automatic_start in [" bootstrap ", " kickstart ", " load ", " enable "]:
                            self.assertNotIn(automatic_start, script)
                    if variant == "rootful" and name == "postinst":
                        cleanup = "rm -f /System/Library/LaunchDaemons/com.apple.fsmod.plist"
                        self.assertEqual([line.strip() for line in script.splitlines()
                                          if line.strip().startswith("rm ")], [cleanup])
                        self.assertLess(script.index(config_check), script.index(cleanup))
                        self.assertLess(script.index(cleanup), script.index(launchctl + " bootstrap"))
                    else:
                        self.assertNotIn("rm ", script)
                    if name != "postinst":
                        self.assertNotIn("fsmod.conf", script)

    @unittest.skipUnless(shutil.which("sh"), "POSIX shell is not available")
    def test_maintainer_scripts_shell_syntax(self):
        for variant in ["rootful", "rootless"]:
            members = unpack_deb(self.build(variant).read_bytes())
            control = unpack_tar(members["control.tar.gz"])
            for name in ["preinst", "prerm", "postinst"]:
                with self.subTest(variant=variant, script=name):
                    script = self.directory / (variant + "-" + name)
                    script.write_bytes(control[name][1])
                    result = subprocess.run([shutil.which("sh"), "-n", str(script)],
                                            capture_output=True, text=True, check=False)
                    self.assertEqual(result.returncode, 0, result.stderr)

    @unittest.skipUnless(shutil.which("sh"), "POSIX shell is not available")
    def test_postinst_validates_before_starting_and_handles_fallback(self):
        for variant, prefix in [("rootful", ""), ("rootless", "var/jb/")]:
            members = unpack_deb(self.build(variant).read_bytes())
            control = unpack_tar(members["control.tar.gz"])
            postinst = control["postinst"][1].decode()
            binary = "/" + prefix + "usr/sbin/fsmod"
            launchctl = "/" + prefix + "bin/launchctl"
            # Only the test copy substitutes commands. Production paths and
            # packaged scripts remain untouched. Functions let this run without
            # root, launchd, or an executable iOS binary on the test machine.
            functions = (
                "test_fsmod() { printf 'check %s\\n' \"$*\" >> \"$FSMOD_TEST_LOG\"; "
                "return \"${FSMOD_TEST_CONFIG_STATUS:-0}\"; }\n"
                "rm() { printf 'remove %s\\n' \"$*\" >> \"$FSMOD_TEST_LOG\"; return 0; }\n"
                "test_launchctl() {\n"
                "  printf 'launchctl %s\\n' \"$*\" >> \"$FSMOD_TEST_LOG\"\n"
                "  case \"$1\" in\n"
                "    bootstrap) return \"${FSMOD_TEST_BOOTSTRAP_STATUS:-0}\" ;;\n"
                "    load) return \"${FSMOD_TEST_LOAD_STATUS:-0}\" ;;\n"
                "  esac\n"
                "  return 0\n"
                "}\n"
            )
            script = functions + postinst.replace(binary, "test_fsmod").replace(launchctl, "test_launchctl")
            script_path = self.directory / (variant + "-postinst-stubbed")
            script_path.write_bytes(script.encode("utf-8"))
            cases = [
                ("invalid-config", "configure", 1, 0, 0, False, 1),
                ("configured", "configure", 0, 0, 0, True, 0),
                ("legacy-fallback", "configure", 0, 1, 0, True, 0),
                ("activation-fails", "configure", 0, 1, 1, True, 1),
                ("non-configure", "abort-upgrade", 0, 0, 0, False, 0),
            ]
            for name, action, config_status, bootstrap_status, load_status, starts, status in cases:
                with self.subTest(variant=variant, case=name):
                    log = self.directory / (variant + "-" + name + ".log")
                    env = dict(os.environ, FSMOD_TEST_LOG=str(log),
                               FSMOD_TEST_CONFIG_STATUS=str(config_status),
                               FSMOD_TEST_BOOTSTRAP_STATUS=str(bootstrap_status),
                               FSMOD_TEST_LOAD_STATUS=str(load_status))
                    result = subprocess.run([shutil.which("sh"), str(script_path), action],
                                            env=env, capture_output=True, text=True, check=False)
                    self.assertEqual(result.returncode, status, result.stderr)
                    calls = log.read_text().splitlines() if log.exists() else []
                    if action != "configure":
                        self.assertEqual(calls, [])
                        continue
                    self.assertEqual(calls[0], "check -T")
                    if not starts:
                        self.assertEqual(calls, ["check -T"])
                        self.assertIn("configuration validation failed", result.stderr)
                        continue
                    plist = "/" + prefix + "Library/LaunchDaemons/com.apple.fsmod.plist"
                    expected = ["check -T", "launchctl bootout system/com.apple.fsmod"]
                    if variant == "rootful":
                        expected += [
                            "launchctl unload /System/Library/LaunchDaemons/com.apple.fsmod.plist",
                            "remove -f /System/Library/LaunchDaemons/com.apple.fsmod.plist",
                        ]
                    expected += [
                        "launchctl enable system/com.apple.fsmod",
                        "launchctl bootstrap system " + plist,
                    ]
                    if bootstrap_status:
                        expected += ["launchctl load -w " + plist]
                    self.assertEqual(calls, expected)
                    if status:
                        self.assertIn("launchd activation failed", result.stderr)

    def test_packages_are_reproducible(self):
        for variant in ["rootful", "rootless"]:
            with self.subTest(variant=variant):
                first = self.build(variant).read_bytes()
                second = self.build(variant).read_bytes()
                self.assertEqual(first, second)

    def test_cli_uses_current_version_and_new_package_name(self):
        self.assertEqual(package.package_version(), "1.0.0")
        result = self.run_cli(self.binary)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sorted(path.name for path in self.directory.glob("*.deb")),
                         ["com.apple.fsmod_1.0.0_iphoneos-arm.deb",
                          "com.apple.fsmod_1.0.0_iphoneos-arm64.deb"])

    def test_cli_package_name_override_keeps_service_identity(self):
        result = self.run_cli(self.binary, "--package-id", "org.example.fsmod",
                              "--variant", "rootful")
        self.assertEqual(result.returncode, 0, result.stderr)
        path = self.directory / "org.example.fsmod_1.0.0_iphoneos-arm.deb"
        members = unpack_deb(path.read_bytes())
        control = unpack_tar(members["control.tar.gz"])
        self.assertEqual(control_fields(control["control"][1])["Package"], "org.example.fsmod")
        data = unpack_tar(members["data.tar.gz"])
        self.assertIn("Library/LaunchDaemons/com.apple.fsmod.plist", data.keys())

    def test_cli_rejects_unsigned_and_non_device_binaries(self):
        for binary, reason in [(fixture_binary(signed=False), "signed with ldid"),
                               (fixture_binary(platform=7), "not a physical iOS device")]:
            with self.subTest(reason=reason):
                result = self.run_cli(binary)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(reason, result.stderr)
        self.assertEqual(list(self.directory.glob("*.deb")), [])


if __name__ == "__main__":
    unittest.main()
