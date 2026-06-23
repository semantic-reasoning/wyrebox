#!/usr/bin/env python3

from pathlib import Path
import stat
import subprocess
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
HELPER_NAME = "wyrebox-postfix-pipe"
INSTALL_BINDIR = "libexec/wyrebox"


def run(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True, text=True)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="wyrebox-postfix-pipe-install-") as tmp:
        tempdir = Path(tmp)
        builddir = tempdir / "build"
        prefix = tempdir / "prefix"
        destdir = tempdir / "destdir"

        run(
            [
                "meson",
                "setup",
                "--prefix",
                str(prefix),
                "--bindir",
                INSTALL_BINDIR,
                str(builddir),
                str(REPO_ROOT),
            ],
            cwd=tempdir,
        )
        run(["meson", "compile", "-C", str(builddir)], cwd=tempdir)
        run(
            [
                "meson",
                "install",
                "-C",
                str(builddir),
                "--destdir",
                str(destdir),
            ],
            cwd=tempdir,
        )

        expected_helper = prefix / INSTALL_BINDIR / HELPER_NAME
        staged_helper = destdir / expected_helper.relative_to(expected_helper.anchor)
        expected_master_cf = (
            prefix
            / "share"
            / "doc"
            / "wyrebox"
            / "examples"
            / "postfix"
            / "master.cf.wyrebox-pipe"
        )
        expected_regexp_transport = (
            prefix
            / "share"
            / "doc"
            / "wyrebox"
            / "examples"
            / "postfix"
            / "transport.wyrebox-pipe-regexp"
        )
        staged_master_cf = (
            destdir / expected_master_cf.relative_to(expected_master_cf.anchor)
        )
        staged_regexp_transport = (
            destdir
            / expected_regexp_transport.relative_to(
                expected_regexp_transport.anchor
            )
        )

        assert staged_helper.is_file(), f"missing helper binary: {staged_helper}"
        assert staged_helper.stat().st_mode & stat.S_IXUSR, (
            f"helper is not executable: {staged_helper}"
        )

        assert staged_master_cf.is_file(), (
            f"missing installed master.cf example: {staged_master_cf}"
        )
        master_cf = staged_master_cf.read_text(encoding="utf-8")
        assert f"argv={expected_helper}" in master_cf, (
            "installed master.cf does not reference the configured helper path"
        )

        assert staged_regexp_transport.is_file(), (
            f"missing installed regexp transport example: {staged_regexp_transport}"
        )
        regexp_transport = staged_regexp_transport.read_text(encoding="utf-8")
        assert "transport_maps = regexp:/etc/postfix/transport" in regexp_transport
        assert "wyrebox-pipe:" in regexp_transport


if __name__ == "__main__":
    main()
