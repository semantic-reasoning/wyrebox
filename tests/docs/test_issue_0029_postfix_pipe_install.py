#!/usr/bin/env python3

from pathlib import Path
import json
import os
import subprocess


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_ROOT_CANDIDATES = [
    Path(os.environ["MESON_BUILD_ROOT"])
    if "MESON_BUILD_ROOT" in os.environ
    else None,
    Path.cwd(),
    REPO_ROOT / "_build",
    REPO_ROOT / "build",
    REPO_ROOT / "builddir",
]


def detect_build_root() -> Path:
    for candidate in BUILD_ROOT_CANDIDATES:
        if candidate is None:
            continue

        if (candidate / "meson-info" / "intro-buildoptions.json").is_file():
            return candidate

    raise AssertionError(
        "could not locate a meson build root with intro-buildoptions.json"
    )


BUILD_ROOT = detect_build_root()

EXAMPLE_INSTALL_RELATIVE_DIR = Path("share") / "doc" / "wyrebox" / "examples" / "postfix"
EXAMPLE_INSTALL_FILES = [
    "README.md",
    "master.cf.wyrebox-pipe",
    "transport.wyrebox-pipe",
]


def load_install_plan() -> dict[str, dict[str, dict[str, object]]]:
    result = subprocess.run(
        ["meson", "introspect", str(BUILD_ROOT), "--install-plan"],
        check=True,
        text=True,
        capture_output=True,
    )

    return json.loads(result.stdout)


def assert_expected_example_files(plan: dict[str, dict[str, dict[str, object]]]) -> None:
    data = plan["data"]

    for filename in EXAMPLE_INSTALL_FILES:
        suffix = f"examples/postfix/{filename}"
        matching_paths = [
            source_path for source_path in data if source_path.endswith(suffix)
        ]
        assert matching_paths, f"missing install plan entry: {suffix}"
        assert len(matching_paths) == 1, (
            f"ambiguous install plan entry for {suffix}: {matching_paths}"
        )
        source_path = matching_paths[0]

        expected_destination = (
            "{datadir}/doc/wyrebox/examples/postfix/" + filename
        )
        assert data[source_path]["destination"] == expected_destination, (
            f"unexpected install destination for {source_path}: "
            f"{data[source_path]['destination']}"
        )


def main() -> None:
    assert_expected_example_files(load_install_plan())


if __name__ == "__main__":
    main()
