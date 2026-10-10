#!/usr/bin/env python3

from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
FIXTURES_DIR = REPO_ROOT / "tests" / "dovecot" / "fixtures"
CHECKER_PATH = REPO_ROOT / "tools" / "check-dovecot-source-contract.py"


def run_checker(
    source_dir: Path | None,
    *,
    env: dict[str, str] | None = None,
    cwd: Path | None = None,
    expect_success: bool = True,
) -> subprocess.CompletedProcess:
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
        return result

    assert result.returncode != 0, (
        "expected checker failure but it succeeded\n"
        f"stdout: {result.stdout}\nstderr: {result.stderr}"
    )
    return result


def assert_checker_fails_with(
    source_dir: Path | None,
    expected_diagnostics: list[str],
    *,
    env: dict[str, str] | None = None,
    cwd: Path | None = None,
    unexpected_diagnostics: list[str] | None = None,
) -> subprocess.CompletedProcess:
    result = run_checker(
        source_dir,
        env=env,
        cwd=cwd,
        expect_success=False,
    )
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


def assert_named_fixture_fails_with(
    fixture_name: str,
    expected_diagnostics: list[str],
) -> None:
    assert_checker_fails_with(
        FIXTURES_DIR / fixture_name,
        expected_diagnostics,
        unexpected_diagnostics=["missing required Dovecot source file"],
    )


def make_flat_include_dir(
    include_dir: Path,
    *,
    version: str = "2.4.2",
    abi_version: str = "2.4.ABIv2",
) -> Path:
    include_dir.mkdir(parents=True)
    for header in (FIXTURES_DIR / "valid-2.4.2" / "src").rglob("*.h"):
        shutil.copy(header, include_dir / header.name)
    config_h = (
        FIXTURES_DIR / "valid-2.4.2" / "build-config-valid" / "config.h"
    ).read_text(encoding="utf-8")
    config_h = config_h.replace('"2.4.ABIv2"', f'"{abi_version}"')
    config_h = config_h.replace(
        "#define DOVECOT_ABI_VERSION",
        f'#define DOVECOT_VERSION "{version}"\n#define DOVECOT_ABI_VERSION',
    )
    (include_dir / "config.h").write_text(config_h, encoding="utf-8")
    return include_dir


def run_include_dir_checker(
    args: list[str],
) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(CHECKER_PATH), *args],
        check=False,
        text=True,
        capture_output=True,
    )


def assert_include_dir_fails_with(
    args: list[str],
    expected_diagnostics: list[str],
) -> None:
    result = run_include_dir_checker(args)
    output = result.stdout + result.stderr
    assert result.returncode != 0, (
        "expected checker failure but it succeeded\n"
        f"stdout: {result.stdout}\nstderr: {result.stderr}"
    )
    for diagnostic in expected_diagnostics:
        assert diagnostic in output, (
            f"expected diagnostic {diagnostic!r}\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )


def run_checker_with_private_header_mutation(
    old: str,
    new: str,
) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as tempdir:
        source_dir = Path(tempdir) / "dovecot-source"
        shutil.copytree(FIXTURES_DIR / "valid-2.4.2", source_dir)
        private_header = source_dir / "src/lib-storage/mail-storage-private.h"
        text = private_header.read_text(encoding="utf-8")
        assert old in text, f"mutation target not found: {old!r}"
        private_header.write_text(text.replace(old, new, 1), encoding="utf-8")
        return run_checker(source_dir, expect_success=False)


def run_checker_with_list_private_header_mutation(
    old: str,
    new: str,
) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as tempdir:
        source_dir = Path(tempdir) / "dovecot-source"
        shutil.copytree(FIXTURES_DIR / "valid-2.4.2", source_dir)
        private_header = source_dir / "src/lib-storage/mailbox-list-private.h"
        text = private_header.read_text(encoding="utf-8")
        assert old in text, f"mutation target not found: {old!r}"
        private_header.write_text(text.replace(old, new, 1), encoding="utf-8")
        return run_checker(source_dir, expect_success=False)


def run_checker_with_storage_header_mutation(
    old: str,
    new: str,
) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as tempdir:
        source_dir = Path(tempdir) / "dovecot-source"
        shutil.copytree(FIXTURES_DIR / "valid-2.4.2", source_dir)
        storage_header = source_dir / "src/lib-storage/mail-storage.h"
        text = storage_header.read_text(encoding="utf-8")
        assert old in text, f"mutation target not found: {old!r}"
        storage_header.write_text(text.replace(old, new, 1), encoding="utf-8")
        return run_checker(source_dir, expect_success=False)


def test_dovecot_source_contract_happy_path() -> None:
    run_checker(FIXTURES_DIR / "valid-2.4.2")


def test_dovecot_source_contract_env_override() -> None:
    run_checker(
        None,
        env={"WYREBOX_DOVECOT_SOURCE_DIR": str(FIXTURES_DIR / "valid-2.4.2")},
    )


def test_dovecot_source_contract_missing_required_file() -> None:
    assert_checker_fails_with(
        FIXTURES_DIR / "missing-file",
        ["missing required Dovecot source file: src/lib-storage/mail-storage-private.h"],
    )


def test_dovecot_source_contract_wrong_version() -> None:
    assert_named_fixture_fails_with(
        "wrong-version",
        ["unexpected Dovecot version 2.4.1; expected 2.4.2"],
    )


def test_dovecot_source_contract_wrong_abi_template() -> None:
    assert_named_fixture_fails_with(
        "wrong-abi-template",
        ["DOVECOT_ABI_VERSION must use template 2.4.ABIv2"],
    )


def test_dovecot_source_contract_missing_vfunc_contract() -> None:
    assert_named_fixture_fails_with(
        "missing-vfunc",
        ["struct mailbox_vfuncs missing required vfunc: open"],
    )


def test_dovecot_source_contract_missing_mailbox_allocation_contract() -> None:
    assert_named_fixture_fails_with(
        "missing-mailbox-allocation",
        ["struct mailbox missing name"],
    )


def test_dovecot_source_contract_missing_mail_uid_contract() -> None:
    result = run_checker_with_storage_header_mutation(
        "unsigned int uid;",
        "unsigned int removed_uid;",
    )
    output = result.stdout + result.stderr
    assert "mail-storage.h: struct mail missing uid" in output


def test_dovecot_source_contract_missing_mail_transaction_refcount() -> None:
    result = run_checker_with_private_header_mutation(
        "unsigned int mail_ref_count;",
        "unsigned int removed_mail_ref_count;",
    )
    output = result.stdout + result.stderr
    assert (
        "mail-storage-private.h: struct mailbox_transaction_context "
        "missing mail_ref_count"
    ) in output


def test_dovecot_source_contract_missing_list_contract() -> None:
    assert_named_fixture_fails_with(
        "missing-list-contract",
        [
            "missing mail_storage_vfuncs.add_list signature",
            "missing mailbox_list_get_storage_name signature",
        ],
    )


def test_dovecot_source_contract_list_iterator_iter_init_signature() -> None:
    result = run_checker_with_list_private_header_mutation(
        "const char *const *patterns",
        "const char **patterns",
    )
    output = result.stdout + result.stderr
    assert "missing mailbox_list_vfuncs.iter_init signature" in output
    assert "missing mailbox_list_vfuncs.iter_next signature" not in output
    assert "missing mailbox_list_vfuncs.iter_deinit signature" not in output


def test_dovecot_source_contract_list_iterator_iter_next_signature() -> None:
    result = run_checker_with_list_private_header_mutation(
        "const struct mailbox_info *(*iter_next)",
        "struct mailbox_info *(*iter_next)",
    )
    output = result.stdout + result.stderr
    assert "missing mailbox_list_vfuncs.iter_init signature" not in output
    assert "missing mailbox_list_vfuncs.iter_next signature" in output
    assert "missing mailbox_list_vfuncs.iter_deinit signature" not in output


def test_dovecot_source_contract_list_iterator_iter_deinit_signature() -> None:
    result = run_checker_with_list_private_header_mutation(
        "int (*iter_deinit)",
        "bool (*iter_deinit)",
    )
    output = result.stdout + result.stderr
    assert "missing mailbox_list_vfuncs.iter_init signature" not in output
    assert "missing mailbox_list_vfuncs.iter_next signature" not in output
    assert "missing mailbox_list_vfuncs.iter_deinit signature" in output


def test_dovecot_source_contract_missing_namespace_user_identity() -> None:
    assert_named_fixture_fails_with(
        "missing-namespace-user-identity",
        [
            "missing mail_namespace user identity pointer",
            "missing mail_user username identity field",
        ],
    )


def test_dovecot_source_contract_missing_plugin_entrypoint() -> None:
    assert_named_fixture_fails_with(
        "missing-plugin-entrypoint",
        ["missing module entrypoint ABI declarations"],
    )


def test_dovecot_source_contract_missing_storage_registration() -> None:
    assert_named_fixture_fails_with(
        "missing-storage-registration",
        ["missing storage registration declaration"],
    )


def test_dovecot_source_contract_missing_source_directory() -> None:
    run_checker(
        None,
        env={"WYREBOX_DOVECOT_SOURCE_DIR": ""},
        cwd=FIXTURES_DIR / "valid-2.4.2",
        expect_success=False,
    )


def test_dovecot_source_contract_include_dir_happy_path() -> None:
    with tempfile.TemporaryDirectory() as tempdir:
        include_dir = make_flat_include_dir(Path(tempdir) / "dovecot")
        result = run_include_dir_checker(["--include-dir", str(include_dir)])
        assert result.returncode == 0, (
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )


def test_dovecot_source_contract_include_dir_missing_header() -> None:
    with tempfile.TemporaryDirectory() as tempdir:
        include_dir = make_flat_include_dir(Path(tempdir) / "dovecot")
        (include_dir / "mail-storage-private.h").unlink()
        assert_include_dir_fails_with(
            ["--include-dir", str(include_dir)],
            ["missing required Dovecot header: mail-storage-private.h"],
        )


def test_dovecot_source_contract_include_dir_wrong_version() -> None:
    with tempfile.TemporaryDirectory() as tempdir:
        include_dir = make_flat_include_dir(
            Path(tempdir) / "dovecot", version="2.4.1"
        )
        assert_include_dir_fails_with(
            ["--include-dir", str(include_dir)],
            ['config.h DOVECOT_VERSION must equal "2.4.2"'],
        )


def test_dovecot_source_contract_include_dir_wrong_abi() -> None:
    with tempfile.TemporaryDirectory() as tempdir:
        include_dir = make_flat_include_dir(
            Path(tempdir) / "dovecot", abi_version="2.4.ABIv1"
        )
        assert_include_dir_fails_with(
            ["--include-dir", str(include_dir)],
            ['config.h DOVECOT_ABI_VERSION must equal "2.4.ABIv2"'],
        )


def test_dovecot_source_contract_include_dir_rejects_source_dir() -> None:
    with tempfile.TemporaryDirectory() as tempdir:
        include_dir = make_flat_include_dir(Path(tempdir) / "dovecot")
        assert_include_dir_fails_with(
            [
                str(FIXTURES_DIR / "valid-2.4.2"),
                "--include-dir",
                str(include_dir),
            ],
            ["--include-dir cannot be combined with a source directory"],
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
        test_dovecot_source_contract_missing_mail_uid_contract,
        test_dovecot_source_contract_missing_mail_transaction_refcount,
        test_dovecot_source_contract_missing_list_contract,
        test_dovecot_source_contract_list_iterator_iter_init_signature,
        test_dovecot_source_contract_list_iterator_iter_next_signature,
        test_dovecot_source_contract_list_iterator_iter_deinit_signature,
        test_dovecot_source_contract_missing_namespace_user_identity,
        test_dovecot_source_contract_missing_plugin_entrypoint,
        test_dovecot_source_contract_missing_storage_registration,
        test_dovecot_source_contract_missing_source_directory,
        test_dovecot_source_contract_include_dir_happy_path,
        test_dovecot_source_contract_include_dir_missing_header,
        test_dovecot_source_contract_include_dir_wrong_version,
        test_dovecot_source_contract_include_dir_wrong_abi,
        test_dovecot_source_contract_include_dir_rejects_source_dir,
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
