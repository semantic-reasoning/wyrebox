#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DOVECOT_CONFIG = Path("/usr/lib/dovecot/dovecot-config")
MISSING_DOVECOT_HINT = (
    "install dovecot-dev and dovecot-imapd or set WYREBOX_DOVECOT_CONFIG to an "
    "installed dovecot-config"
)


def run(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True, text=True)


def main() -> None:
    dovecot_config = Path(
        os.environ.get("WYREBOX_DOVECOT_CONFIG", DEFAULT_DOVECOT_CONFIG)
    )
    if not dovecot_config.is_file():
        raise SystemExit(
            "Dovecot IMAP fetch recipe prerequisites are missing: "
            f"{MISSING_DOVECOT_HINT}"
        )

    with tempfile.TemporaryDirectory(prefix="wyrebox-dovecot-imap-fetch-recipe-") as tmp:
        tempdir = Path(tmp)
        builddir = tempdir / "build"

        run(
            [
                "meson",
                "setup",
                "-Ddovecot_backend=enabled",
                f"-Ddovecot_config={dovecot_config}",
                str(builddir),
                str(REPO_ROOT),
            ],
            cwd=tempdir,
        )
        run(
            ["meson", "test", "-C", str(builddir), "dovecot plugin symbols"],
            cwd=tempdir,
        )


if __name__ == "__main__":
    main()
