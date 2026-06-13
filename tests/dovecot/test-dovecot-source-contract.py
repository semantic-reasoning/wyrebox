#!/usr/bin/env python3

from pathlib import Path
import os
import subprocess
import sys


REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES_DIR = REPO_ROOT / "tests" / "dovecot" / "fixtures"
CHECKER_PATH = REPO_ROOT / "tools" / "check-dovecot-source-contract.py"


def run_checker(source_dir: Path | None, *, env: dict[str, str] | None = None,
                cwd: Path | None = None, expect_success: bool = True) -> None:
    args = [str(CHECKER_PATH)]
    if source_dir is not None:
        args.append(str(source_dir))

    call_env = dict(os.environ)
    if env:
        call_env.update(env)

    result = subprocess.run(
        [sys.executable, *args],
        env=call_env,
        cwd=str(cwd) if cwd is not None else None,
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


def test_dovecot_source_contract_happy_path() -> None:
    run_checker(FIXTURES_DIR / "valid-2.3.21.1")


def test_dovecot_source_contract_env_override() -> None:
    run_checker(
        None,
        env={"WYREBOX_DOVECOT_SOURCE_DIR": str(FIXTURES_DIR / "valid-2.3.21.1")},
    )


def test_dovecot_source_contract_missing_required_file() -> None:
    run_checker(FIXTURES_DIR / "missing-file", expect_success=False)


def test_dovecot_source_contract_wrong_version() -> None:
    run_checker(FIXTURES_DIR / "wrong-version", expect_success=False)


def test_dovecot_source_contract_wrong_abi_template() -> None:
    run_checker(FIXTURES_DIR / "wrong-abi-template", expect_success=False)


def test_dovecot_source_contract_missing_vfunc_contract() -> None:
    run_checker(FIXTURES_DIR / "missing-vfunc", expect_success=False)


def test_dovecot_source_contract_missing_mailbox_allocation_contract() -> None:
    run_checker(FIXTURES_DIR / "missing-mailbox-allocation", expect_success=False)


def test_dovecot_source_contract_missing_list_contract() -> None:
    run_checker(FIXTURES_DIR / "missing-list-contract", expect_success=False)


def test_dovecot_source_contract_missing_namespace_user_identity() -> None:
    run_checker(
        FIXTURES_DIR / "missing-namespace-user-identity",
        expect_success=False,
    )


def test_dovecot_source_contract_missing_plugin_entrypoint() -> None:
    run_checker(FIXTURES_DIR / "missing-plugin-entrypoint", expect_success=False)


def test_dovecot_source_contract_missing_storage_registration() -> None:
    run_checker(
        FIXTURES_DIR / "missing-storage-registration",
        expect_success=False,
    )


def test_dovecot_source_contract_missing_source_directory() -> None:
    run_checker(
        None,
        env={"WYREBOX_DOVECOT_SOURCE_DIR": ""},
        cwd=FIXTURES_DIR / "valid-2.3.21.1",
        expect_success=False,
    )


def main() -> None:
    test_functions = [
        test_dovecot_source_contract_happy_path,
        test_dovecot_source_contract_env_override,
        test_dovecot_source_contract_missing_required_file,
        test_dovecot_source_contract_wrong_version,
        test_dovecot_source_contract_wrong_abi_template,
        test_dovecot_source_contract_missing_vfunc_contract,
        test_dovecot_source_contract_missing_mailbox_allocation_contract,
        test_dovecot_source_contract_missing_list_contract,
        test_dovecot_source_contract_missing_namespace_user_identity,
        test_dovecot_source_contract_missing_plugin_entrypoint,
        test_dovecot_source_contract_missing_storage_registration,
        test_dovecot_source_contract_missing_source_directory,
    ]
    failures: list[tuple[str, Exception]] = []
    for test in test_functions:
        try:
            test()
        except Exception as exc:  # pragma: no cover - exercised by direct-run validation
            failures.append((test.__name__, exc))

    if failures:
        lines = [f" - {name}: {error}" for name, error in failures]
        raise SystemExit("dovecot source contract tests failed:\n" + "\n".join(lines))

    print("dovecot source contract tests passed:", len(test_functions))


if __name__ == "__main__":
    main()
