#!/usr/bin/env python3

import os
import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"
CONTAINERFILE = REPO_ROOT / "ci" / "ubuntu-26.04" / "Containerfile"
RUN_SCRIPT = REPO_ROOT / "tools" / "ci" / "run-ubuntu-26.04.sh"

JOB_NAME = "ubuntu-26-04-dovecot"
JOB_NEEDS = {
    "pr-lint.yml": "pr-lint",
    "main-ci.yml": "formatting",
}
REQUIRED_PACKAGES = [
    "build-essential",
    "ca-certificates",
    "capnproto",
    "dovecot-dev",
    "dovecot-imapd",
    "git",
    "libcapnp-dev",
    "libglib2.0-dev",
    "meson",
    "ninja-build",
    "pkg-config",
    "postfix",
    "python3",
    "unzip",
]
CAPNP_ENABLED_FLAGS = ["-Dcapnp=enabled", "-Dcapnp_serialization=enabled"]
SYSTEM_FLAGS = [
    "-Ddovecot_backend=enabled",
    "-Ddovecot_config=/usr/lib/dovecot/dovecot-config",
]
FIXTURE_FLAGS = [
    "-Ddovecot_backend=enabled",
    "-Ddovecot_source_dir=tests/dovecot/fixtures/valid-2.4.2",
    "-Ddovecot_build_dir=tests/dovecot/fixtures/valid-2.4.2/build-config-valid",
]


def read_jobs(workflow: str) -> dict[str, str]:
    path = WORKFLOWS_DIR / workflow
    assert path.is_file(), f"missing workflow: {path}"
    text = path.read_text(encoding="utf-8")
    _, sep, jobs_text = text.partition("\njobs:\n")
    assert sep, f"{workflow}: missing top-level jobs key"

    jobs: dict[str, str] = {}
    matches = list(re.finditer(r"^  ([A-Za-z0-9_-]+):\n", jobs_text, re.M))
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else None
        jobs[match.group(1)] = jobs_text[match.end():end]
    return jobs


def meson_setup_commands(text: str) -> list[str]:
    joined = re.sub(r"\\\n\s*", " ", text)
    return [line for line in joined.splitlines() if "meson setup" in line]


def check_containerfile() -> None:
    assert CONTAINERFILE.is_file(), f"missing {CONTAINERFILE}"
    text = CONTAINERFILE.read_text(encoding="utf-8")
    assert re.search(r"^FROM\s+(docker\.io/library/)?ubuntu:26\.04\s*$", text,
                     re.M), "Containerfile must start from ubuntu:26.04"
    assert "DEBIAN_FRONTEND=noninteractive" in text, (
        "Containerfile must install packages non-interactively"
    )
    assert re.search(r"postfix/main_mailer_type\s+select\s+No configuration",
                     text), "Containerfile must preseed the postfix debconf"
    for package in REQUIRED_PACKAGES:
        assert re.search(rf"(?<![\w.-]){re.escape(package)}(?![\w.-])", text), (
            f"Containerfile must install {package}"
        )


def check_run_script() -> None:
    assert RUN_SCRIPT.is_file(), f"missing {RUN_SCRIPT}"
    assert os.access(RUN_SCRIPT, os.X_OK), f"{RUN_SCRIPT} must be executable"
    text = RUN_SCRIPT.read_text(encoding="utf-8")
    assert re.search(r"^set -eu", text, re.M), "run script must use set -eu"

    setups = meson_setup_commands(text)
    system = [s for s in setups if all(flag in s for flag in SYSTEM_FLAGS)]
    fixture = [s for s in setups if all(flag in s for flag in FIXTURE_FLAGS)]
    assert len(system) == 1, "run script must configure system Dovecot mode once"
    assert len(fixture) == 1, "run script must configure fixture Dovecot mode once"
    for setup in system + fixture:
        for flag in CAPNP_ENABLED_FLAGS:
            assert flag in setup, f"run script setup must use {flag}: {setup}"

    assert text.count("meson compile") >= 2, "run script must build both modes"
    assert text.count("meson test") >= 2, "run script must test both modes"
    assert "--print-errorlogs" in text, "run script must print test error logs"


def check_workflow_job(workflow: str, needs: str) -> None:
    jobs = read_jobs(workflow)
    assert JOB_NAME in jobs, f"{workflow}: missing job {JOB_NAME}"
    job = jobs[JOB_NAME]
    assert re.search(rf"^\s+needs:\s*{re.escape(needs)}\s*$", job, re.M), (
        f"{workflow}:{JOB_NAME} must need {needs}"
    )
    assert "ci/ubuntu-26.04/Containerfile" in job, (
        f"{workflow}:{JOB_NAME} must build ci/ubuntu-26.04/Containerfile"
    )
    assert "tools/ci/run-ubuntu-26.04.sh" in job, (
        f"{workflow}:{JOB_NAME} must run tools/ci/run-ubuntu-26.04.sh"
    )


def main() -> None:
    check_containerfile()
    check_run_script()
    for workflow, needs in JOB_NEEDS.items():
        check_workflow_job(workflow, needs)


if __name__ == "__main__":
    main()
