#!/usr/bin/env python3

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
MESON_BUILD_FILE = REPO_ROOT / "wyrebox" / "meson.build"

EXAMPLE_FILES = [
    "../examples/postfix/README.md",
    "../examples/postfix/master.cf.wyrebox-pipe",
    "../examples/postfix/transport.wyrebox-pipe",
]


def read_build_file() -> str:
    assert MESON_BUILD_FILE.is_file(), (
        f"missing meson build file: {MESON_BUILD_FILE}"
    )
    return MESON_BUILD_FILE.read_text(encoding="utf-8")


def assert_example_install_block(build_file: str) -> None:
    assert "install_data(" in build_file, "missing example install_data block"
    assert "if capnp_serialization_found" in build_file, (
        "postfix example install block must stay within the capnp guard"
    )

    for example_file in EXAMPLE_FILES:
        assert example_file in build_file, (
            f"missing example file in install_data block: {example_file}"
        )

    assert "join_paths(" in build_file, "missing install_dir join_paths call"
    for fragment in ["'doc'", "meson.project_name()", "'examples'", "'postfix'"]:
        assert fragment in build_file, f"missing install_dir fragment: {fragment}"

    assert build_file.count("install_data(") == 1, (
        "postfix example bundle install block should remain singular"
    )


def main() -> None:
    assert_example_install_block(read_build_file())


if __name__ == "__main__":
    main()
