#!/usr/bin/env python3

from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path


REQUIRED_SYMBOLS = {
    "wyrebox_plugin_deinit": "T",
    "wyrebox_plugin_init": "T",
    "wyrebox_plugin_version": "D",
}

FORBIDDEN_DEFINED_SYMBOLS = {
    "mail_storage_class_register",
    "mail_storage_class_unregister",
}


def derive_dovecot_module_name(module_path: Path) -> str:
    name = module_path.name
    if name.startswith("lib"):
        name = name[3:]

    index = 0
    while index < len(name) and name[index].isdigit():
        index += 1
    if index < len(name) and name[index] == "_":
        name = name[index + 1:]

    if ".so" in name:
        name = name[:name.index(".so")]

    return name


def run_tool(tool: str, *args: str) -> str:
    try:
        completed = subprocess.run(
            [tool, *args],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except subprocess.CalledProcessError as exc:
        raise AssertionError(
            f"{tool} failed with exit status {exc.returncode}:\n{exc.stderr}"
        ) from exc
    return completed.stdout


def parse_dynamic_symbols(nm_output: str) -> dict[str, str]:
    symbols: dict[str, str] = {}
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) == 3:
            _, symbol_type, name = parts
        elif len(parts) == 2:
            symbol_type, name = parts
        else:
            continue
        symbols[name] = symbol_type
    return symbols


def assert_required_symbols(module_path: Path) -> None:
    nm = shutil.which("nm")
    if nm is None:
        raise AssertionError("nm is required to verify Dovecot plugin symbols")

    module_name = derive_dovecot_module_name(module_path)
    expected_symbols = {
        f"{module_name}_deinit": "T",
        f"{module_name}_init": "T",
        f"{module_name}_version": "D",
    }
    if expected_symbols != REQUIRED_SYMBOLS:
        raise AssertionError(
            f"module filename {module_path.name!r} implies symbols "
            f"{sorted(expected_symbols)}, but test requires "
            f"{sorted(REQUIRED_SYMBOLS)}"
        )

    symbols = parse_dynamic_symbols(run_tool(nm, "-D", "--defined-only",
                                             str(module_path)))
    for symbol_name, expected_type in REQUIRED_SYMBOLS.items():
        actual_type = symbols.get(symbol_name)
        if actual_type != expected_type:
            raise AssertionError(
                f"expected dynamic symbol {symbol_name} type {expected_type}, "
                f"got {actual_type!r}; exported symbols: {sorted(symbols)}"
            )

    unexpected = sorted(set(symbols) - set(REQUIRED_SYMBOLS))
    if unexpected:
        raise AssertionError(f"unexpected exported symbols: {unexpected}")


def assert_no_dovecot_host_symbols_defined(module_path: Path) -> None:
    nm = shutil.which("nm")
    if nm is None:
        raise AssertionError("nm is required to verify Dovecot plugin symbols")

    symbols = parse_dynamic_symbols(run_tool(nm, "--defined-only",
                                             str(module_path)))
    forbidden = sorted(FORBIDDEN_DEFINED_SYMBOLS & set(symbols))
    if forbidden:
        raise AssertionError(
            "Dovecot plugin must not define host storage symbols: " +
            ", ".join(forbidden)
        )


def assert_readelf_sees_dynamic_symbols(module_path: Path) -> None:
    readelf = shutil.which("readelf")
    if readelf is None:
        raise AssertionError("readelf is required to verify Dovecot plugin symbols")

    output = run_tool(readelf, "--wide", "--dyn-syms", str(module_path))
    missing = [
        symbol_name
        for symbol_name in REQUIRED_SYMBOLS
        if symbol_name not in output
    ]
    if missing:
        raise AssertionError(f"readelf did not list required symbols: {missing}")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test-dovecot-plugin-symbols.py MODULE_PATH")

    module_path = Path(sys.argv[1])
    if not module_path.is_file():
        raise AssertionError(f"Dovecot plugin module not found: {module_path}")

    assert_required_symbols(module_path)
    assert_no_dovecot_host_symbols_defined(module_path)
    assert_readelf_sees_dynamic_symbols(module_path)
    print(f"Dovecot plugin symbols verified: {module_path}")


if __name__ == "__main__":
    main()
