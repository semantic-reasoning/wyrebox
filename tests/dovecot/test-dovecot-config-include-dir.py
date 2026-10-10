#!/usr/bin/env python3

from pathlib import Path
import subprocess
import sys
import tempfile


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL = REPO_ROOT / "tools" / "dovecot-config-include-dir.py"


def run_tool(config_text: str) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory(prefix="wyrebox-dovecot-config-") as tmp:
        config = Path(tmp) / "dovecot-config"
        config.write_text(config_text, encoding="utf-8")
        return subprocess.run(
            [sys.executable, str(TOOL), str(config)],
            check=False,
            text=True,
            capture_output=True,
        )


def assert_include_dir(config_text: str, expected: str) -> None:
    result = run_tool(config_text)
    if result.returncode != 0:
        raise AssertionError(
            f"expected success, got {result.returncode}: {result.stderr}"
        )
    if result.stdout != expected + "\n":
        raise AssertionError(f"unexpected stdout: {result.stdout!r}")


def assert_fails_with(config_text: str, message: str) -> None:
    result = run_tool(config_text)
    if result.returncode == 0:
        raise AssertionError(f"expected failure, got stdout {result.stdout!r}")
    if message not in result.stderr:
        raise AssertionError(
            f"expected {message!r} in stderr, got {result.stderr!r}"
        )


def test_plain_value() -> None:
    assert_include_dir(
        "DOVECOT_INSTALLED=yes\n"
        "LIBDOVECOT='-L/usr/lib/dovecot -ldovecot'\n"
        "LIBDOVECOT_INCLUDE=-I/usr/include/dovecot\n"
        "dovecot_moduledir=/usr/lib/dovecot/modules\n",
        "/usr/include/dovecot",
    )


def test_quoted_value() -> None:
    assert_include_dir(
        'LIBDOVECOT_INCLUDE="-I/opt/dovecot/include/dovecot"\n',
        "/opt/dovecot/include/dovecot",
    )


def test_missing_variable() -> None:
    assert_fails_with(
        "DOVECOT_INSTALLED=yes\n",
        "LIBDOVECOT_INCLUDE is not set in",
    )


def test_rejects_multiple_dirs() -> None:
    assert_fails_with(
        'LIBDOVECOT_INCLUDE="-I/a -I/b"\n',
        "LIBDOVECOT_INCLUDE must name exactly one -I directory",
    )


def test_rejects_non_include_token() -> None:
    assert_fails_with(
        'LIBDOVECOT_INCLUDE="-I/a -DFOO"\n',
        "unsupported LIBDOVECOT_INCLUDE token: -DFOO",
    )


def test_missing_file() -> None:
    result = subprocess.run(
        [sys.executable, str(TOOL), "/nonexistent/dovecot-config"],
        check=False,
        text=True,
        capture_output=True,
    )
    if result.returncode == 0 or "cannot read" not in result.stderr:
        raise AssertionError(f"unexpected result: {result!r}")


def main() -> int:
    tests = [
        test_plain_value,
        test_quoted_value,
        test_missing_variable,
        test_rejects_multiple_dirs,
        test_rejects_non_include_token,
        test_missing_file,
    ]
    for test in tests:
        test()
    print(f"dovecot-config include dir tests passed: {len(tests)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
