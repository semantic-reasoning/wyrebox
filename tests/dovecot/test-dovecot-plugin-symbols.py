#!/usr/bin/env python3

from __future__ import annotations

import re
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

# Full-library bleed the thin daemon-client link must not pull into the plugin.
# Anchored prefixes spare API request helpers such as
# wyrebox_daemon_duckdb_query_template_*.
FORBIDDEN_DEFINED_PREFIXES = (
    "duckdb_",
    "wyrebox_duckdb_",
    "wyrebox_local_object_store",
    "wyrebox_journal_",
)

# RUNPATH entries that only appear when the plugin still links the full stack.
FORBIDDEN_RUNPATH_TOKENS = (
    "duckdb",
    "wirelog",
)

FORBIDDEN_NEEDED_RE = re.compile(r"libduckdb", re.IGNORECASE)


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


def parse_dynamic_needed_and_runpaths(readelf_output: str) -> tuple[
        list[str], list[str]]:
    needed: list[str] = []
    runpaths: list[str] = []
    for line in readelf_output.splitlines():
        if "(NEEDED)" in line:
            start = line.find("[")
            end = line.rfind("]")
            if start != -1 and end != -1 and end > start:
                needed.append(line[start + 1:end])
        if "(RUNPATH)" in line or "(RPATH)" in line:
            start = line.find("[")
            end = line.rfind("]")
            if start != -1 and end != -1 and end > start:
                runpaths.append(line[start + 1:end])
    return needed, runpaths


def assert_no_full_stack_link_bleed(module_path: Path) -> None:
    readelf = shutil.which("readelf")
    if readelf is None:
        raise AssertionError("readelf is required to verify Dovecot plugin symbols")
    nm = shutil.which("nm")
    if nm is None:
        raise AssertionError("nm is required to verify Dovecot plugin symbols")

    needed, runpaths = parse_dynamic_needed_and_runpaths(
        run_tool(readelf, "-d", str(module_path)))
    duckdb_needed = sorted(
        name for name in needed if FORBIDDEN_NEEDED_RE.search(name))
    if duckdb_needed:
        raise AssertionError(
            "Dovecot plugin must not DT_NEEDED DuckDB: " +
            ", ".join(duckdb_needed))

    forbidden_runpaths: list[str] = []
    for runpath in runpaths:
        lowered = runpath.lower()
        for token in FORBIDDEN_RUNPATH_TOKENS:
            if token in lowered:
                forbidden_runpaths.append(runpath)
                break
    if forbidden_runpaths:
        raise AssertionError(
            "Dovecot plugin RUNPATH/RPATH must not reference full-stack "
            "subprojects (duckdb/wirelog): " + "; ".join(forbidden_runpaths))

    symbols = parse_dynamic_symbols(run_tool(nm, "--defined-only",
                                             str(module_path)))
    forbidden = sorted(
        name for name in symbols
        if any(name.startswith(prefix) for prefix in FORBIDDEN_DEFINED_PREFIXES))
    if forbidden:
        raise AssertionError(
            "Dovecot plugin must not define full-stack storage symbols: " +
            ", ".join(forbidden))


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: test-dovecot-plugin-symbols.py MODULE_PATH")

    module_path = Path(sys.argv[1])
    if not module_path.is_file():
        raise AssertionError(f"Dovecot plugin module not found: {module_path}")

    assert_required_symbols(module_path)
    assert_no_dovecot_host_symbols_defined(module_path)
    assert_readelf_sees_dynamic_symbols(module_path)
    assert_no_full_stack_link_bleed(module_path)
    print(f"Dovecot plugin symbols verified: {module_path}")


if __name__ == "__main__":
    main()
