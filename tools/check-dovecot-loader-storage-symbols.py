#!/usr/bin/env python3

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


REQUIRED_SYMBOLS = {
    "mail_storage_class_register",
    "mail_storage_class_unregister",
}


def run_nm(archive_path: Path) -> str:
    nm = shutil.which("nm")
    if nm is None:
        raise AssertionError("nm is required to inspect Dovecot loader symbols")

    completed = subprocess.run(
        [nm, "-g", "--defined-only", str(archive_path)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return completed.stdout


def parse_defined_symbols(nm_output: str) -> set[str]:
    symbols: set[str] = set()
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) == 3:
            _, symbol_type, name = parts
        elif len(parts) == 2:
            symbol_type, name = parts
        else:
            continue

        if symbol_type.upper() == "U":
            continue
        symbols.add(name)
    return symbols


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: check-dovecot-loader-storage-symbols.py LOADER_ARCHIVE"
        )

    archive_path = Path(sys.argv[1])
    if not archive_path.is_file():
        raise AssertionError(f"Dovecot loader archive not found: {archive_path}")

    defined_symbols = parse_defined_symbols(run_nm(archive_path))
    missing = sorted(REQUIRED_SYMBOLS - defined_symbols)
    if missing:
        print(
            "missing Dovecot storage registration symbols: " +
            ", ".join(missing),
            file=sys.stderr,
        )
        return 1

    print(
        "Dovecot storage registration symbols found: " +
        ", ".join(sorted(REQUIRED_SYMBOLS))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
