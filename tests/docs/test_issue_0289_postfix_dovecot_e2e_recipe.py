#!/usr/bin/env python3

import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
DOVECOT_CONFIG = Path("/usr/lib/dovecot/dovecot-config")


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
                f"-Ddovecot_config={DOVECOT_CONFIG}",
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
                "dovecot plugin symbols",
            ],
            cwd=tempdir,
        )


if __name__ == "__main__":
    main()
