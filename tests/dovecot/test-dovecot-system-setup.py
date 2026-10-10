#!/usr/bin/env python3

from pathlib import Path
import ctypes
import os
import subprocess
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DOVECOT_CONFIG = Path("/usr/lib/dovecot/dovecot-config")
EXPECTED_ABI_VERSION = b"2.4.ABIv2"
MESON_SKIP = 77


def meson_setup(builddir: Path, *options: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["meson", "setup", *options, str(builddir), str(REPO_ROOT)],
        check=False,
        text=True,
        capture_output=True,
    )


def assert_setup_rejected(tempdir: Path, name: str, message: str,
                          *options: str) -> None:
    result = meson_setup(tempdir / name, *options)
    if result.returncode == 0:
        raise AssertionError(f"meson setup unexpectedly accepted {options}")
    if message not in result.stdout + result.stderr:
        raise AssertionError(
            f"expected {message!r} for {options}\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )


def list_meson_tests(builddir: Path) -> set[str]:
    completed = subprocess.run(
        ["meson", "test", "-C", str(builddir), "--list"],
        check=True,
        text=True,
        capture_output=True,
    )
    return {
        line.partition(":")[2].strip()
        for line in completed.stdout.splitlines()
        if line.strip()
    }


def read_exported_abi_version(plugin: Path, dovecot_libdir: Path) -> bytes:
    for library in ("libdovecot.so.0", "libdovecot-storage.so.0"):
        ctypes.CDLL(str(dovecot_libdir / library), mode=ctypes.RTLD_GLOBAL)
    module = ctypes.CDLL(str(plugin), mode=os.RTLD_LAZY)
    return ctypes.c_char_p.in_dll(module, "wyrebox_plugin_version").value


def main() -> int:
    dovecot_config = Path(
        os.environ.get("WYREBOX_DOVECOT_CONFIG", DEFAULT_DOVECOT_CONFIG)
    )
    if not dovecot_config.is_file():
        print(f"skipping: no installed Dovecot config at {dovecot_config}")
        return MESON_SKIP

    system_options = [
        "-Ddovecot_backend=enabled",
        f"-Ddovecot_config={dovecot_config}",
    ]
    fixture_dir = REPO_ROOT / "tests" / "dovecot" / "fixtures" / "valid-2.4.2"

    with tempfile.TemporaryDirectory(prefix="wyrebox-dovecot-system-") as tmp:
        tempdir = Path(tmp)

        assert_setup_rejected(
            tempdir,
            "with-source-dir",
            "-Ddovecot_config cannot be combined with",
            *system_options,
            f"-Ddovecot_source_dir={fixture_dir}",
        )
        assert_setup_rejected(
            tempdir,
            "with-loader-smoke",
            "Dovecot loader smoke is not supported with -Ddovecot_config",
            *system_options,
            "-Ddovecot_loader_smoke=enabled",
        )

        builddir = tempdir / "build"
        result = meson_setup(builddir, *system_options)
        if result.returncode != 0:
            raise AssertionError(
                f"system Dovecot setup failed\n"
                f"stdout: {result.stdout}\nstderr: {result.stderr}"
            )

        tests = list_meson_tests(builddir)
        if "dovecot plugin symbols" not in tests:
            raise AssertionError("system mode must register dovecot plugin symbols")
        if "dovecot plugin mailbox smoke" in tests:
            raise AssertionError(
                "the fixture-only mailbox smoke test must not build in system mode"
            )

        subprocess.run(
            ["meson", "compile", "-C", str(builddir), "wyrebox_plugin"],
            check=True,
            text=True,
            capture_output=True,
        )
        plugin = builddir / "wyrebox" / "wyrebox_plugin.so"
        version = read_exported_abi_version(plugin, dovecot_config.parent)
        if version != EXPECTED_ABI_VERSION:
            raise AssertionError(
                f"wyrebox_plugin_version is {version!r}, "
                f"expected {EXPECTED_ABI_VERSION!r}"
            )

    print(f"system Dovecot plugin exports {EXPECTED_ABI_VERSION.decode()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
