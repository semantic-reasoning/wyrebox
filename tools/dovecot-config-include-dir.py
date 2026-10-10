#!/usr/bin/env python3
"""Print the Dovecot include directory named by an installed dovecot-config."""

from __future__ import annotations

from pathlib import Path
import shlex
import sys


INCLUDE_VARIABLE = "LIBDOVECOT_INCLUDE"


def read_include_value(config: Path) -> str:
    try:
        text = config.read_text(encoding="utf-8")
    except OSError as error:
        raise ValueError(f"cannot read {config}: {error}") from error

    for line in text.splitlines():
        name, separator, value = line.partition("=")
        if separator and name.strip() == INCLUDE_VARIABLE:
            return value

    raise ValueError(f"{INCLUDE_VARIABLE} is not set in {config}")


def parse_include_dir(value: str) -> str:
    try:
        tokens = [token for word in shlex.split(value) for token in word.split()]
    except ValueError as error:
        raise ValueError(f"cannot parse {INCLUDE_VARIABLE}: {error}") from error

    include_dirs: list[str] = []
    for token in tokens:
        if not token.startswith("-I") or token == "-I":
            raise ValueError(f"unsupported {INCLUDE_VARIABLE} token: {token}")
        include_dirs.append(token[2:])

    if len(include_dirs) != 1:
        raise ValueError(
            f"{INCLUDE_VARIABLE} must name exactly one -I directory, "
            f"got {len(include_dirs)}"
        )
    return include_dirs[0]


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} /path/to/dovecot-config", file=sys.stderr)
        return 2

    try:
        include_dir = parse_include_dir(read_include_value(Path(sys.argv[1])))
    except ValueError as error:
        print(f"dovecot-config include dir lookup failed: {error}", file=sys.stderr)
        return 1

    print(include_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
