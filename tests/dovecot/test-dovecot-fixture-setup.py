#!/usr/bin/env python3

from pathlib import Path
import json
import subprocess
import sys
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_DIR = REPO_ROOT / "tests" / "dovecot" / "fixtures" / "valid-2.4.2"
RETIRED_TESTS = {
    "dovecot plugin symbols",
    "dovecot plugin mailbox smoke",
    "dovecot plugin loader smoke",
    "dovecot plugin loader smoke (shimmed registration)",
}
RETIRED_OPTIONS = {
    "dovecot_loader_smoke",
    "dovecot_loader_archive",
    "dovecot_loader_extra_link_args",
}


def introspect(builddir: Path, what: str) -> list[dict]:
    completed = subprocess.run(
        ["meson", "introspect", what, str(builddir)],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(completed.stdout)


def main() -> None:
    # Extra arguments are Meson -D options from the enclosing build, such as
    # -Dduckdb_root on architectures without a bundled DuckDB prebuilt.
    forwarded_options = sys.argv[1:]

    with tempfile.TemporaryDirectory(prefix="wyrebox-dovecot-fixture-") as tmp:
        builddir = Path(tmp) / "build"
        result = subprocess.run(
            [
                "meson",
                "setup",
                *forwarded_options,
                "-Ddovecot_backend=enabled",
                f"-Ddovecot_source_dir={FIXTURE_DIR}",
                f"-Ddovecot_build_dir={FIXTURE_DIR / 'build-config-valid'}",
                str(builddir),
                str(REPO_ROOT),
            ],
            check=False,
            text=True,
            capture_output=True,
        )
        if result.returncode != 0:
            raise AssertionError(
                "fixture Dovecot setup failed\n"
                f"stdout: {result.stdout}\nstderr: {result.stderr}"
            )

        targets = {target["name"] for target in introspect(builddir, "--targets")}
        if "wyrebox_plugin" in targets:
            raise AssertionError("fixture mode must not compile the Dovecot plugin")

        tests = {test["name"] for test in introspect(builddir, "--tests")}
        stale = sorted(tests & RETIRED_TESTS)
        if stale:
            raise AssertionError(f"fixture mode registers plugin tests: {stale}")

        options = {option["name"] for option in introspect(builddir, "--buildoptions")}
        stale = sorted(options & RETIRED_OPTIONS)
        if stale:
            raise AssertionError(f"retired Dovecot loader options remain: {stale}")

    print("fixture Dovecot mode runs contract checks only")


if __name__ == "__main__":
    main()
