#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
WRAP_PATH = REPO_ROOT / "subprojects" / "libchronoid.wrap"
MESON_PATH = REPO_ROOT / "meson.build"
WYREBOX_MESON_PATH = REPO_ROOT / "wyrebox" / "meson.build"
CONTRACT_PATH = REPO_ROOT / "docs" / "contracts" / "libchronoid-dependency.md"
README_PATH = REPO_ROOT / "README.md"


def chronoid_block(meson: str) -> str:
    start = meson.index("# libchronoid")
    end = meson.index("summary('libchronoid'", start)
    return meson[start:end]


def main() -> None:
    wrap = WRAP_PATH.read_text(encoding="utf-8")
    assert "[wrap-git]" in wrap
    assert re.search(
        r"^url = https://github.com/semantic-reasoning/libchronoid\.git$",
        wrap, re.MULTILINE)
    assert re.search(
        r"^revision = 2188221565f91a67b0c238bb8ad96fd61060c6ee$",
        wrap, re.MULTILINE), "libchronoid wrap must pin v1.2.0 by commit"

    meson = MESON_PATH.read_text(encoding="utf-8")
    assert "meson_version: '>=1.1.0'" in meson
    block = chronoid_block(meson)
    assert "dependency('libchronoid'" in block
    assert "subproject(" in block and "'libchronoid'" in block
    assert "'tests=false'" in block
    assert "'cli=false'" in block
    assert ".get_shared_lib()" in block
    assert ".get_static_lib()" not in block

    wyrebox_meson = WYREBOX_MESON_PATH.read_text(encoding="utf-8")
    assert ("chronoid_dep.partial_dependency(compile_args: true, "
            "includes: true)") in wyrebox_meson, (
        "wyrebox_lib must not link libchronoid into every binary")
    assert re.search(r"'wyreboxd',\s*'daemon/wyreboxd-main\.c',\s*"
                     r"dependencies: \[wyrebox_dep, chronoid_dep\]",
                     wyrebox_meson), "wyreboxd must link libchronoid"

    contract = CONTRACT_PATH.read_text(encoding="utf-8")
    for phrase in [
        "LGPL-3.0-or-later",
        "shared library",
        "LICENSE.MIT",
        "subprojects/libchronoid.wrap",
        "meson subprojects download",
        "Only `wyreboxd`",
    ]:
        assert phrase in contract, f"libchronoid contract missing: {phrase}"

    readme = README_PATH.read_text(encoding="utf-8")
    license_section = readme[readme.index("## License"):]
    assert "libchronoid" in license_section
    assert "LGPL-3.0-or-later" in license_section


if __name__ == "__main__":
    main()
