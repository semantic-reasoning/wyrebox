#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
MISSING_DOVECOT_HINT = (
    "WYREBOX_DOVECOT_SOURCE_DIR, WYREBOX_DOVECOT_BUILD_DIR, and "
    "WYREBOX_DOVECOT_LOADER_ARCHIVE must be set to exercise the Dovecot "
    "backend recipe."
)


def run(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True, text=True)


def main() -> None:
    dovecot_source_dir = os.environ.get("WYREBOX_DOVECOT_SOURCE_DIR")
    dovecot_build_dir = os.environ.get("WYREBOX_DOVECOT_BUILD_DIR")
    dovecot_loader_archive = os.environ.get("WYREBOX_DOVECOT_LOADER_ARCHIVE")

    if not dovecot_source_dir or not dovecot_build_dir or not dovecot_loader_archive:
        print(f"skipping Dovecot IMAP fetch recipe: {MISSING_DOVECOT_HINT}")
        return

    with tempfile.TemporaryDirectory(prefix="wyrebox-dovecot-imap-fetch-recipe-") as tmp:
        tempdir = Path(tmp)
        builddir = tempdir / "build"

        run(
            [
                "meson",
                "setup",
                "-Ddovecot_backend=enabled",
                f"-Ddovecot_source_dir={dovecot_source_dir}",
                f"-Ddovecot_build_dir={dovecot_build_dir}",
                "-Ddovecot_loader_smoke=enabled",
                f"-Ddovecot_loader_archive={dovecot_loader_archive}",
                str(builddir),
                str(REPO_ROOT),
            ],
            cwd=tempdir,
        )
        run(
            ["meson", "test", "-C", str(builddir), "dovecot plugin load smoke"],
            cwd=tempdir,
        )
        run(
            ["meson", "test", "-C", str(builddir), "dovecot plugin mailbox smoke"],
            cwd=tempdir,
        )


if __name__ == "__main__":
    main()
