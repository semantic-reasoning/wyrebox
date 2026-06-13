#!/usr/bin/env python3

from pathlib import Path
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES_DIR = REPO_ROOT / "tests" / "dovecot" / "fixtures"
VALID_SOURCE_FIXTURE = FIXTURES_DIR / "valid-2.3.21.1"
BUILD_CHECKER_PATH = REPO_ROOT / "tools" / "check-dovecot-build-contract.py"


def run_checker(source_dir: Path, build_dir: Path, *, expect_success: bool = True) -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(BUILD_CHECKER_PATH),
            str(source_dir),
            str(build_dir),
        ],
        check=False,
        text=True,
        capture_output=True,
    )

    if expect_success:
        assert result.returncode == 0, (
            "expected checker success but failed\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )
        return

    assert result.returncode != 0, (
        "expected checker failure but it succeeded\n"
        f"stdout: {result.stdout}\nstderr: {result.stderr}"
    )


def test_dovecot_build_contract_happy_path() -> None:
    run_checker(
        VALID_SOURCE_FIXTURE,
        FIXTURES_DIR / "valid-2.3.21.1" / "build-config-valid",
    )


def test_dovecot_build_contract_missing_file() -> None:
    run_checker(
        VALID_SOURCE_FIXTURE,
        FIXTURES_DIR / "valid-2.3.21.1" / "build-config-missing-file",
        expect_success=False,
    )


def test_dovecot_build_contract_invalid_macros() -> None:
    run_checker(
        VALID_SOURCE_FIXTURE,
        FIXTURES_DIR / "valid-2.3.21.1" / "build-config-missing-uoff-selector",
        expect_success=False,
    )


def test_dovecot_build_contract_wrong_abi() -> None:
    run_checker(
        VALID_SOURCE_FIXTURE,
        FIXTURES_DIR / "valid-2.3.21.1" / "build-config-wrong-abi",
        expect_success=False,
    )


def test_dovecot_build_contract_missing_source_directory() -> None:
    run_checker(
        Path('/definitely-not-a-real-dovecot-source-tree'),
        FIXTURES_DIR / "valid-2.3.21.1" / "build-config-valid",
        expect_success=False,
    )


def main() -> None:
    test_functions = [
        test_dovecot_build_contract_happy_path,
        test_dovecot_build_contract_missing_file,
        test_dovecot_build_contract_invalid_macros,
        test_dovecot_build_contract_wrong_abi,
        test_dovecot_build_contract_missing_source_directory,
    ]
    failures: list[tuple[str, Exception]] = []
    for test in test_functions:
        try:
            test()
        except Exception as exc:  # pragma: no cover - exercised by direct-run validation
            failures.append((test.__name__, exc))

    if failures:
        lines = [f" - {name}: {error}" for name, error in failures]
        raise SystemExit(
            "dovecot build contract tests failed:\n" + "\n".join(lines),
        )

    print("dovecot build contract tests passed:", len(test_functions))


if __name__ == "__main__":
    main()
