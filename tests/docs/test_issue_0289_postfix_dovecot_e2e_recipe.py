#!/usr/bin/env python3

import os
import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
DOVECOT_CONFIG = Path("/usr/lib/dovecot/dovecot-config")
DOVECOT_IMAP = Path("/usr/lib/dovecot/imap")


def run(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True, text=True)


def main() -> None:
    if not DOVECOT_CONFIG.is_file() or not os.access(DOVECOT_IMAP, os.X_OK):
        raise SystemExit(
            "Postfix to Dovecot recipe prerequisites are missing: install "
            "dovecot-dev and dovecot-imapd"
        )
    if os.geteuid() == 0:
        raise SystemExit(
            "Postfix to Dovecot recipe must run as a non-root user: Dovecot "
            "imap refuses to run as root without mail_uid"
        )

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
                "dovecot imap real daemon",
            ],
            cwd=tempdir,
        )


if __name__ == "__main__":
    main()
