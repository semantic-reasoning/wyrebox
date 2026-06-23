#!/usr/bin/env python3

import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DOVECOT_SOURCE_DIR = REPO_ROOT / "tests" / "dovecot" / "fixtures" / (
    "valid-2.3.21.1"
)
DEFAULT_DOVECOT_BUILD_DIR = (
    DEFAULT_DOVECOT_SOURCE_DIR / "build-config-valid"
)


def run(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True, text=True)


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="wyrebox-postfix-dovecot-e2e-recipe-") as tmp:
        tempdir = Path(tmp)
        builddir = tempdir / "build"

        run(
            [
                "meson",
                "setup",
                "-Ddovecot_backend=enabled",
                f"-Ddovecot_source_dir={DEFAULT_DOVECOT_SOURCE_DIR}",
                f"-Ddovecot_build_dir={DEFAULT_DOVECOT_BUILD_DIR}",
                str(builddir),
                str(REPO_ROOT),
            ],
            cwd=tempdir,
        )
        run(
            [
                "meson",
                "test",
                "-C",
                str(builddir),
                "postfix pipe real daemon delivery",
            ],
            cwd=tempdir,
        )
        run(
            [
                "meson",
                "test",
                "-C",
                str(builddir),
                "dovecot plugin mailbox smoke",
            ],
            cwd=tempdir,
        )


if __name__ == "__main__":
    main()
