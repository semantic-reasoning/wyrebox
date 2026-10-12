#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
DOVECOT_CONFIG = Path("/usr/lib/dovecot/dovecot-config")
DOVECOT_MASTER = Path("/usr/sbin/dovecot")
DOVECOT_IMAP_LOGIN = Path("/usr/lib/dovecot/imap-login")
DOVECOT_IMAP = Path("/usr/lib/dovecot/imap")
MISSING_DOVECOT_HINT = (
    "install dovecot-dev, dovecot-core and dovecot-imapd"
)


def run(command: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    subprocess.run(command, cwd=cwd, env=env, check=True, text=True)


def main() -> None:
    if (
        not DOVECOT_CONFIG.is_file()
        or not os.access(DOVECOT_MASTER, os.X_OK)
        or not os.access(DOVECOT_IMAP_LOGIN, os.X_OK)
        or not os.access(DOVECOT_IMAP, os.X_OK)
    ):
        raise SystemExit(
            "Postfix to Dovecot IMAP e2e recipe prerequisites are missing: "
            f"{MISSING_DOVECOT_HINT}"
        )
    if os.geteuid() == 0:
        raise SystemExit(
            "Postfix to Dovecot IMAP e2e recipe must run as a non-root user: "
            "the test starts its own Dovecot master as the current user"
        )

    with tempfile.TemporaryDirectory(prefix="wyrebox-imap-e2e-recipe-") as tmp:
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
                "--print-errorlogs",
                "postfix dovecot imap e2e",
            ],
            cwd=tempdir,
            env={**os.environ, "WYREBOX_E2E_REQUIRE_RUNTIME": "1"},
        )


if __name__ == "__main__":
    main()
