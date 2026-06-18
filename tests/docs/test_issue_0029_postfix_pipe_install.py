#!/usr/bin/env python3

from pathlib import Path
import json
import os
import subprocess
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_ROOT = Path(os.environ.get("MESON_BUILD_ROOT", REPO_ROOT / "build"))
INTRO_BUILD_OPTIONS_PATH = BUILD_ROOT / "meson-info" / "intro-buildoptions.json"

EXAMPLE_INSTALL_RELATIVE_DIR = Path("share") / "doc" / "wyrebox" / "examples" / "postfix"
EXAMPLE_INSTALL_FILES = [
    "README.md",
    "master.cf.wyrebox-pipe",
    "transport.wyrebox-pipe",
]


def read_prefix() -> str:
    assert INTRO_BUILD_OPTIONS_PATH.is_file(), (
        f"missing build introspection file: {INTRO_BUILD_OPTIONS_PATH}"
    )

    build_options = json.loads(INTRO_BUILD_OPTIONS_PATH.read_text(encoding="utf-8"))
    for option in build_options:
        if option.get("name") == "prefix":
            return option["value"]

    raise AssertionError("build introspection file does not define prefix")


def install_to(destdir: Path) -> None:
    subprocess.run(
        ["meson", "install", "-C", str(BUILD_ROOT), "--destdir", str(destdir),
         "--no-rebuild"],
        check=True,
        text=True,
    )


def assert_expected_example_files(installed_root: Path) -> None:
    example_root = installed_root / read_prefix().lstrip("/") / EXAMPLE_INSTALL_RELATIVE_DIR
    assert example_root.is_dir(), f"missing installed example directory: {example_root}"

    for filename in EXAMPLE_INSTALL_FILES:
        path = example_root / filename
        assert path.is_file(), f"missing installed example file: {path}"


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="wyrebox-install-smoke-") as tempdir:
        install_root = Path(tempdir)
        install_to(install_root)
        assert_expected_example_files(install_root)


if __name__ == "__main__":
    main()
