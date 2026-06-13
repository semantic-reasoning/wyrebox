#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "duckdb-dependency.md"
MESON_PATH = REPO_ROOT / "meson.build"
WRAP_PATH = REPO_ROOT / "subprojects" / "duckdb-prebuilt-linux.wrap"
WRONG_WRAP_PATH = REPO_ROOT / "subprojects" / "duckdb.wrap"
GITIGNORE_PATH = REPO_ROOT / "subprojects" / ".gitignore"
PACKAGEFILE_MESON_PATH = (
    REPO_ROOT / "subprojects" / "packagefiles" / "duckdb-prebuilt-linux" / "meson.build"
)

REQUIRED_SECTIONS = [
    "# DuckDB Dependency Contract",
    "## Status",
    "## Scope",
    "## Dependency Policy",
    "## Platform Constraint",
    "## Wrap Contract",
    "## Commit Hygiene",
]


def section_map(text: str) -> dict[str, str]:
    matches = list(re.finditer(r"^## .+$", text, flags=re.MULTILINE))
    sections: dict[str, str] = {}
    for index, match in enumerate(matches):
        start = match.end()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        sections[match.group(0)] = text[start:end].strip()
    return sections


def assert_in_section(sections: dict[str, str], section: str, needle: str) -> None:
    assert needle in sections[section], f"{section} missing contract text: {needle}"


def assert_section_matches(sections: dict[str, str], section: str, pattern: str) -> None:
    flexible_pattern = pattern.replace(" ", r"\s+")
    assert re.search(flexible_pattern, sections[section], re.IGNORECASE | re.DOTALL), (
        f"{section} missing contract pattern: {pattern}"
    )


def assert_forbidden(text: str, pattern: str) -> None:
    assert not re.search(pattern, text, re.IGNORECASE | re.DOTALL), (
        f"contract contains contradictory language: {pattern}"
    )


def main() -> None:
    assert CONTRACT_PATH.is_file(), f"missing contract: {CONTRACT_PATH}"
    contract_text = CONTRACT_PATH.read_text(encoding="utf-8")

    missing_sections = [section for section in REQUIRED_SECTIONS if section not in contract_text]
    assert not missing_sections, "missing required sections: " + ", ".join(missing_sections)

    sections = section_map(contract_text)

    assert_section_matches(
        sections,
        "## Status",
        r"Accepted\.",
    )

    for line in [
        "System `duckdb` providers resolved via pkg-config",
        "subproject('duckdb-prebuilt-linux')",
        "not accepted",
    ]:
        assert_in_section(sections, "## Dependency Policy", line)

    assert_in_section(
        sections,
        "## Scope",
        "defines only DuckDB acquisition policy during Meson configuration",
    )

    for line in [
        "Linux x86_64",
    ]:
        assert_in_section(sections, "## Platform Constraint", line)

    assert_in_section(
        sections,
        "## Wrap Contract",
        "subprojects/duckdb-prebuilt-linux.wrap",
    )
    assert_in_section(sections, "## Wrap Contract", "No `duckdb.wrap` file is used")

    for line in [
        "This contract forbids committing generated or extracted third-party subproject",
        "only `subprojects/duckdb-prebuilt-linux.wrap`",
        "subprojects/packagefiles/duckdb-prebuilt-linux/meson.build",
    ]:
        assert_in_section(sections, "## Commit Hygiene", line)

    for forbidden in [
        r"fallback.*is acceptable",
        r"system duckdb provider is accepted",
        r"depends on `duckdb` via pkg-config",
        r"optional dependency",
    ]:
        assert_forbidden(contract_text, forbidden)

    assert WRAP_PATH.is_file(), f"missing wrap file: {WRAP_PATH}"
    assert not WRONG_WRAP_PATH.is_file(), "legacy `duckdb.wrap` was found"

    wrap_text = WRAP_PATH.read_text(encoding="utf-8")
    for needle in [
        "[wrap-file]",
        "directory = duckdb-prebuilt-linux-amd64",
        "lead_directory_missing = true",
        "patch_directory = duckdb-prebuilt-linux",
        "[provide]",
        "duckdb-prebuilt-linux = duckdb_dep",
    ]:
        assert needle in wrap_text, f"wrap file missing {needle}"

    assert GITIGNORE_PATH.is_file(), "missing subprojects/.gitignore"
    gitignore_text = GITIGNORE_PATH.read_text(encoding="utf-8")
    for line in [
        "/*",
        "!/.gitignore",
        "!/*.wrap",
        "!/packagefiles/",
        "/packagefiles/*",
        "!/packagefiles/duckdb-prebuilt-linux/",
        "!/packagefiles/duckdb-prebuilt-linux/meson.build",
    ]:
        assert line in gitignore_text, f"missing gitignore control: {line}"

    meson_text = MESON_PATH.read_text(encoding="utf-8")
    assert not re.search(
        r"dependency\(\s*['\"]duckdb['\"]",
        meson_text,
    ), "meson should not use direct duckdb dependency lookup"
    assert not re.search(
        r"dependency\(\s*['\"]duckdb-prebuilt-linux['\"]",
        meson_text,
    ), "meson should not use dependency lookup for wrapped duckdb"

    assert "duckdb_subproject = subproject('duckdb-prebuilt-linux')" in meson_text
    assert (
        "duckdb_subproject_dep = duckdb_subproject.get_variable('duckdb_dep')"
        in meson_text
    )
    assert "dependencies: duckdb_subproject_dep" in meson_text
    assert "DuckDB is required" in meson_text
    assert "supports Linux x86_64 only" in meson_text
    assert "cc.links(" in meson_text

    duckdb_block_start = meson_text.find("duckdb_api_probe")
    duckdb_block_end = meson_text.find("wirelog_runtime_option", duckdb_block_start)
    assert duckdb_block_start != -1 and duckdb_block_end != -1, (
        "unable to locate DuckDB build policy block"
    )
    duckdb_block = meson_text[duckdb_block_start:duckdb_block_end]
    assert "subproject('duckdb-prebuilt-linux')" in duckdb_block
    assert (
        "duckdb_api_usable = cc.links" in duckdb_block
        and "not duckdb_api_usable" in duckdb_block
    )
    assert "x86_64" in duckdb_block

    assert "Use the wrapped DuckDB subproject directly" in meson_text

    assert PACKAGEFILE_MESON_PATH.is_file(), (
        f"missing DuckDB packagefile Meson shim: {PACKAGEFILE_MESON_PATH}"
    )
    packagefile_text = PACKAGEFILE_MESON_PATH.read_text(encoding="utf-8")
    for needle in [
        "fs = import('fs')",
        "meson.current_source_dir() / 'libduckdb.so'",
        "DuckDB prebuilt wrap did not provide libduckdb.so",
        "cc.find_library('duckdb'",
        "dirs: meson.current_source_dir()",
        "required: true",
    ]:
        assert needle in packagefile_text, f"DuckDB packagefile missing {needle}"

    assert not re.search(
        r"cc\.find_library\(\s*['\"]duckdb['\"]\s*,\s*required:\s*true",
        packagefile_text,
        flags=re.DOTALL,
    ), "DuckDB packagefile must not search system library paths without dirs"


if __name__ == "__main__":
    main()
