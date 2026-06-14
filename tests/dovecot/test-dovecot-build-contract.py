#!/usr/bin/env python3

from pathlib import Path
import os
import shutil
import shlex
import subprocess
import sys
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES_DIR = REPO_ROOT / "tests" / "dovecot" / "fixtures"
VALID_SOURCE_FIXTURE = FIXTURES_DIR / "valid-2.3.21.1"
BUILD_CHECKER_PATH = REPO_ROOT / "tools" / "check-dovecot-build-contract.py"
CC_SHIM_LOG_ENV = "WYREBOX_CC_SHIM_LOG"


def run_checker(
    source_dir: Path,
    build_dir: Path,
    *,
    env: dict[str, str] | None = None,
    expect_success: bool = True,
) -> subprocess.CompletedProcess:
    call_env = dict(os.environ)
    if env is not None:
        call_env.update(env)

    result = subprocess.run(
        [
            sys.executable,
            str(BUILD_CHECKER_PATH),
            str(source_dir),
            str(build_dir),
        ],
        env=call_env,
        check=False,
        text=True,
        capture_output=True,
    )

    if expect_success:
        assert result.returncode == 0, (
            "expected checker success but failed\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )
        return result

    assert result.returncode != 0, (
        "expected checker failure but it succeeded\n"
        f"stdout: {result.stdout}\nstderr: {result.stderr}"
    )
    return result


def assert_checker_fails_with(
    source_dir: Path,
    build_dir: Path,
    expected_diagnostics: list[str],
    *,
    unexpected_diagnostics: list[str] | None = None,
) -> subprocess.CompletedProcess:
    result = run_checker(source_dir, build_dir, expect_success=False)
    output = result.stdout + result.stderr
    for diagnostic in expected_diagnostics:
        assert diagnostic in output, (
            f"expected diagnostic {diagnostic!r}\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )
    for diagnostic in unexpected_diagnostics or []:
        assert diagnostic not in output, (
            f"unexpected diagnostic {diagnostic!r}\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )
    return result


def run_checker_with_source_mutation(
    header_relative_path: Path,
    old: str,
    new: str,
) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as tempdir:
        source_dir = Path(tempdir) / "dovecot-source"
        shutil.copytree(VALID_SOURCE_FIXTURE, source_dir)
        header = source_dir / header_relative_path
        text = header.read_text(encoding="utf-8")
        assert old in text, f"mutation target not found: {old!r}"
        header.write_text(text.replace(old, new, 1), encoding="utf-8")
        return run_checker(
            source_dir,
            VALID_SOURCE_FIXTURE / "build-config-valid",
            expect_success=False,
        )


def _build_cc_shim(shim_dir: Path) -> Path:
    shim_path = shim_dir / "wyrebox-cc-shim.py"
    shim_path.write_text(
        """#!/usr/bin/env python3
import os
import pathlib
import sys

pathlib.Path(os.environ["%s"]).write_text(
    "\\n".join(sys.argv[1:]),
    encoding="utf-8",
)
"""
        % CC_SHIM_LOG_ENV,
        encoding="utf-8",
    )
    shim_path.chmod(0o755)
    return shim_path


def test_dovecot_build_contract_happy_path() -> None:
    run_checker(
        VALID_SOURCE_FIXTURE,
        FIXTURES_DIR / "valid-2.3.21.1" / "build-config-valid",
    )


def test_dovecot_build_contract_missing_file() -> None:
    assert_checker_fails_with(
        VALID_SOURCE_FIXTURE,
        FIXTURES_DIR / "valid-2.3.21.1" / "build-config-missing-file",
        ["build directory is missing required config.h"],
    )


def test_dovecot_build_contract_invalid_macros() -> None:
    assert_checker_fails_with(
        VALID_SOURCE_FIXTURE,
        FIXTURES_DIR / "valid-2.3.21.1" / "build-config-missing-uoff-selector",
        ["config.h must define one uoff_t selector macro"],
    )


def test_dovecot_build_contract_wrong_abi() -> None:
    assert_checker_fails_with(
        VALID_SOURCE_FIXTURE,
        FIXTURES_DIR / "valid-2.3.21.1" / "build-config-wrong-abi",
        ['config.h DOVECOT_ABI_VERSION must equal "2.3.ABIv21(2.3.21.1)"'],
    )


def test_dovecot_build_contract_missing_mailbox_vfunc_shape() -> None:
    result = run_checker_with_source_mutation(
        Path("src/lib-storage/mail-storage-private.h"),
        "void (*free) (struct mailbox * box);",
        "int (*free) (struct mailbox * box);",
    )
    output = result.stdout + result.stderr
    assert "mailbox_vfuncs::free signature mismatch" in output
    assert "fatal error:" not in output


def test_dovecot_build_contract_missing_mailbox_allocation_surface() -> None:
    result = run_checker_with_source_mutation(
        Path("src/lib-storage/mail-storage-private.h"),
        "struct mailbox\n{\n  const char *name;",
        "struct mailbox\n{\n  char *name;",
    )
    output = result.stdout + result.stderr
    assert "mailbox struct::name expected const char pointer" in output
    assert "fatal error:" not in output


def test_dovecot_build_contract_missing_list_contract() -> None:
    result = run_checker_with_source_mutation(
        Path("src/lib-storage/mail-storage-private.h"),
        "void (*add_list) (struct mail_storage * storage, struct mailbox_list * list);",
        "int (*add_list) (struct mail_storage * storage, struct mailbox_list * list);",
    )
    output = result.stdout + result.stderr
    assert "mail_storage_vfuncs::add_list signature mismatch" in output
    assert "fatal error:" not in output


def test_dovecot_build_contract_list_iterator_iter_init_signature() -> None:
    result = run_checker_with_source_mutation(
        Path("src/lib-storage/mailbox-list-private.h"),
        "const char *const *patterns",
        "const char **patterns",
    )
    output = result.stdout + result.stderr
    assert "mailbox_list_vfuncs::iter_init signature mismatch" in output
    assert "mailbox_list_vfuncs::iter_next signature mismatch" not in output
    assert "mailbox_list_vfuncs::iter_deinit signature mismatch" not in output
    assert "fatal error:" not in output


def test_dovecot_build_contract_list_iterator_iter_next_signature() -> None:
    result = run_checker_with_source_mutation(
        Path("src/lib-storage/mailbox-list-private.h"),
        "const struct mailbox_info *(*iter_next)",
        "struct mailbox_info *(*iter_next)",
    )
    output = result.stdout + result.stderr
    assert "mailbox_list_vfuncs::iter_init signature mismatch" not in output
    assert "mailbox_list_vfuncs::iter_next signature mismatch" in output
    assert "mailbox_list_vfuncs::iter_deinit signature mismatch" not in output
    assert "fatal error:" not in output


def test_dovecot_build_contract_list_iterator_iter_deinit_signature() -> None:
    result = run_checker_with_source_mutation(
        Path("src/lib-storage/mailbox-list-private.h"),
        "int (*iter_deinit)",
        "bool (*iter_deinit)",
    )
    output = result.stdout + result.stderr
    assert "mailbox_list_vfuncs::iter_init signature mismatch" not in output
    assert "mailbox_list_vfuncs::iter_next signature mismatch" not in output
    assert "mailbox_list_vfuncs::iter_deinit signature mismatch" in output
    assert "fatal error:" not in output


def test_dovecot_build_contract_missing_source_directory() -> None:
    assert_checker_fails_with(
        Path('/definitely-not-a-real-dovecot-source-tree'),
        FIXTURES_DIR / "valid-2.3.21.1" / "build-config-valid",
        ["source directory not found: /definitely-not-a-real-dovecot-source-tree"],
    )


def test_dovecot_build_contract_cc_wrapper_and_args() -> None:
    with tempfile.TemporaryDirectory() as shim_directory:
        shim_dir = Path(shim_directory)
        shim_path = _build_cc_shim(shim_dir)
        log_path = shim_dir / "wyrebox-cc-shim.log"
        cc_spec = (
            f"{shim_path} --wrapper-arg "
            "'wrapped compiler arg with spaces'"
        )

        run_checker(
            VALID_SOURCE_FIXTURE,
            FIXTURES_DIR / "valid-2.3.21.1" / "build-config-valid",
            env={"CC": cc_spec, CC_SHIM_LOG_ENV: str(log_path)},
            expect_success=True,
        )

        actual_args = log_path.read_text(encoding="utf-8").splitlines()
        parsed_cc_args = shlex.split(cc_spec)[1:]

        fixed_probe_args = [
            "-std=gnu11",
            "-fsyntax-only",
            "-Werror=incompatible-pointer-types",
            f"-I{FIXTURES_DIR / 'valid-2.3.21.1' / 'build-config-valid'}",
            f"-I{VALID_SOURCE_FIXTURE / 'src' / 'lib-index'}",
            f"-I{VALID_SOURCE_FIXTURE / 'src' / 'lib'}",
            f"-I{VALID_SOURCE_FIXTURE / 'src' / 'lib-mail'}",
            f"-I{VALID_SOURCE_FIXTURE / 'src' / 'lib-storage'}",
        ]

        assert actual_args[: len(parsed_cc_args)] == parsed_cc_args, (
            f"unexpected wrapper arg order: {actual_args}"
        )
        assert actual_args[len(parsed_cc_args): len(parsed_cc_args) + len(fixed_probe_args)] == fixed_probe_args, (
            f"unexpected fixed probe args: {actual_args}"
        )


def main() -> None:
    test_functions = [
        test_dovecot_build_contract_happy_path,
        test_dovecot_build_contract_missing_file,
        test_dovecot_build_contract_invalid_macros,
        test_dovecot_build_contract_wrong_abi,
        test_dovecot_build_contract_missing_mailbox_vfunc_shape,
        test_dovecot_build_contract_missing_mailbox_allocation_surface,
        test_dovecot_build_contract_missing_list_contract,
        test_dovecot_build_contract_list_iterator_iter_init_signature,
        test_dovecot_build_contract_list_iterator_iter_next_signature,
        test_dovecot_build_contract_list_iterator_iter_deinit_signature,
        test_dovecot_build_contract_missing_source_directory,
        test_dovecot_build_contract_cc_wrapper_and_args,
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
