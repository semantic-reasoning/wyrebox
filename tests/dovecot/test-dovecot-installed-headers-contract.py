#!/usr/bin/env python3

from pathlib import Path
import os
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKERS = [
    REPO_ROOT / "tools" / "check-dovecot-source-contract.py",
    REPO_ROOT / "tools" / "check-dovecot-build-contract.py",
]
DEFAULT_INCLUDE_DIR = Path("/usr/include/dovecot")
MESON_SKIP = 77


def main() -> int:
    include_dir = Path(
        os.environ.get("WYREBOX_DOVECOT_INCLUDE_DIR", DEFAULT_INCLUDE_DIR)
    )
    if not (include_dir / "config.h").is_file():
        print(f"skipping: no installed Dovecot headers in {include_dir}")
        return MESON_SKIP

    for checker in CHECKERS:
        result = subprocess.run(
            [sys.executable, str(checker), "--include-dir", str(include_dir)],
            check=False,
            text=True,
            capture_output=True,
        )
        if result.returncode != 0:
            raise SystemExit(
                f"{checker.name} rejected {include_dir}\n"
                f"stdout: {result.stdout}\nstderr: {result.stderr}"
            )
        print(result.stdout.strip())

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
