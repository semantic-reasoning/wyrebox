#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DOVECOT_SOURCE_DIR = REPO_ROOT / "tests" / "dovecot" / "fixtures" / (
    "valid-2.3.21.1"
)
DEFAULT_DOVECOT_BUILD_DIR = (
    DEFAULT_DOVECOT_SOURCE_DIR / "build-config-valid"
)
DEFAULT_DOVECOT_LOADER_ARCHIVE = Path("/usr/lib/dovecot/libdovecot-storage.so")
MISSING_DOVECOT_HINT = (
    "set WYREBOX_DOVECOT_SOURCE_DIR/WYREBOX_DOVECOT_BUILD_DIR or keep the "
    "repository defaults available, and provide a Dovecot loader archive"
)


def run(command: list[str], cwd: Path) -> None:
    subprocess.run(command, cwd=cwd, check=True, text=True)


def list_meson_tests(builddir: Path) -> set[str]:
    completed = subprocess.run(
        ["meson", "test", "-C", str(builddir), "--list"],
        check=True,
        text=True,
        capture_output=True,
    )
    return {line.strip() for line in completed.stdout.splitlines() if line.strip()}


def resolve_dovecot_path(
    env_var: str,
    default_path: Path,
    *,
    require_file: bool = True,
) -> Path | None:
    value = os.environ.get(env_var)
    if value:
        path = Path(value)
    else:
        path = default_path

    if require_file and not path.is_file():
        return None

    if not require_file and not path.is_dir():
        return None

    return path


def main() -> None:
    dovecot_source_dir = resolve_dovecot_path(
        "WYREBOX_DOVECOT_SOURCE_DIR",
        DEFAULT_DOVECOT_SOURCE_DIR,
        require_file=False,
    )
    dovecot_build_dir = resolve_dovecot_path(
        "WYREBOX_DOVECOT_BUILD_DIR",
        DEFAULT_DOVECOT_BUILD_DIR,
        require_file=False,
    )
    dovecot_loader_archive = resolve_dovecot_path(
        "WYREBOX_DOVECOT_LOADER_ARCHIVE",
        DEFAULT_DOVECOT_LOADER_ARCHIVE,
    )

    if dovecot_source_dir is None or dovecot_build_dir is None or (
        dovecot_loader_archive is None
    ):
        raise SystemExit(
            "Dovecot IMAP fetch recipe prerequisites are missing: "
            f"{MISSING_DOVECOT_HINT}"
        )

    with tempfile.TemporaryDirectory(prefix="wyrebox-dovecot-imap-fetch-recipe-") as tmp:
        tempdir = Path(tmp)
        builddir = tempdir / "build"

        run(
            [
                "meson",
                "setup",
                "-Ddovecot_backend=enabled",
                f"-Ddovecot_source_dir={dovecot_source_dir}",
                f"-Ddovecot_build_dir={dovecot_build_dir}",
                "-Ddovecot_loader_smoke=enabled",
                f"-Ddovecot_loader_archive={dovecot_loader_archive}",
                str(builddir),
                str(REPO_ROOT),
            ],
            cwd=tempdir,
        )
        available_tests = list_meson_tests(builddir)

        if "dovecot plugin load smoke" in available_tests:
            run(
                ["meson", "test", "-C", str(builddir), "dovecot plugin load smoke"],
                cwd=tempdir,
            )
        else:
            print("dovecot plugin load smoke unavailable in this environment")

        if "dovecot plugin mailbox smoke" in available_tests:
            run(
                ["meson", "test", "-C", str(builddir), "dovecot plugin mailbox smoke"],
                cwd=tempdir,
            )
        else:
            print("dovecot plugin mailbox smoke unavailable in this environment")


if __name__ == "__main__":
    main()
