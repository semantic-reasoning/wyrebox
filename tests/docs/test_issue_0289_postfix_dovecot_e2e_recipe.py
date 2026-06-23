#!/usr/bin/env python3

from pathlib import Path
import subprocess


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_DIR = REPO_ROOT / "build"
REQUIRED_TESTS = [
    "wyrebox:wyreboxd main",
    "wyrebox:postfix lmtp executable",
    "wyrebox:postfix lmtp delivery",
    "wyrebox:dovecot plugin mailbox smoke",
]


def main() -> None:
    subprocess.run(
        [
            "meson",
            "test",
            "-C",
            str(BUILD_DIR),
            *REQUIRED_TESTS,
            "--print-errorlogs",
        ],
        check=True,
        text=True,
    )


if __name__ == "__main__":
    main()
