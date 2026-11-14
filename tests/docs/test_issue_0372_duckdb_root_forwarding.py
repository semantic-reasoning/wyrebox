#!/usr/bin/env python3

from pathlib import Path
import re


REPO_ROOT = Path(__file__).resolve().parents[2]
MESON_BUILD = REPO_ROOT / "meson.build"
DOVECOT_MESON = REPO_ROOT / "tests" / "dovecot" / "meson.build"

RECIPE_SCRIPTS = [
    "tests/docs/test_issue_0029_postfix_pipe_install_smoke.py",
    "tests/docs/test_issue_0289_postfix_dovecot_e2e_recipe.py",
    "tests/docs/test_issue_0292_postfix_to_dovecot_e2e_recipe.py",
    "tests/docs/test_issue_0293_dovecot_imap_fetch_recipe.py",
    "tests/docs/test_issue_0315_postfix_dovecot_imap_e2e_recipe.py",
    "tests/dovecot/test-dovecot-system-setup.py",
    "tests/dovecot/test-dovecot-fixture-setup.py",
]


def main() -> None:
    meson = MESON_BUILD.read_text(encoding="utf-8")
    dovecot = DOVECOT_MESON.read_text(encoding="utf-8")

    assert "nested_setup_forwarded_args = []" in meson, (
        "meson.build must define nested_setup_forwarded_args"
    )
    assert re.search(
        r"nested_setup_forwarded_args \+= \['-Dduckdb_root=' \+ duckdb_root\]",
        meson,
    ), "nested_setup_forwarded_args must append -Dduckdb_root"

    for script in RECIPE_SCRIPTS:
        assert script.split("/")[-1] in meson or script.split("/")[-1] in dovecot, (
            f"{script} must remain registered"
        )
        assert "nested_setup_forwarded_args" in meson or script.startswith(
            "tests/dovecot/"
        )
        text = (REPO_ROOT / script).read_text(encoding="utf-8")
        assert "sys.argv[1:]" in text, f"{script} must splice enclosing -D options"
        assert (
            "*setup_options" in text
            or "*forwarded_options" in text
            or "*FORWARDED_SETUP_OPTIONS" in text
        ), f"{script} must pass spliced options into meson setup"

    for needle in [
        "test_issue_0029_postfix_pipe_install_smoke.py",
        "test_issue_0289_postfix_dovecot_e2e_recipe.py",
        "test_issue_0292_postfix_to_dovecot_e2e_recipe.py",
        "test_issue_0293_dovecot_imap_fetch_recipe.py",
        "test_issue_0315_postfix_dovecot_imap_e2e_recipe.py",
    ]:
        assert re.search(
            rf"{re.escape(needle)}.*\n.*\] \+ nested_setup_forwarded_args",
            meson,
            re.DOTALL,
        ), f"meson.build must forward nested_setup_forwarded_args to {needle}"

    assert "nested_setup_forwarded_args" in dovecot, (
        "dovecot setup tests must use nested_setup_forwarded_args"
    )


if __name__ == "__main__":
    main()
