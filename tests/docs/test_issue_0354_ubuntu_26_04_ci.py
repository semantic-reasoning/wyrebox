#!/usr/bin/env python3

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"
REMOVED_PATHS = [
    REPO_ROOT / "ci" / "ubuntu-26.04" / "Containerfile",
    REPO_ROOT / "tools" / "ci" / "run-ubuntu-26.04.sh",
]

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
    "libcapnp-dev",
    "libglib2.0-dev",
    "meson",
    "ninja-build",
    "pkg-config",
    "postfix",
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


def check_workflow_job(workflow: str, needs: str) -> None:
    jobs = read_jobs(workflow)
    assert JOB_NAME in jobs, f"{workflow}: missing job {JOB_NAME}"
    job = jobs[JOB_NAME]
    where = f"{workflow}:{JOB_NAME}"

    assert re.search(r"^\s+runs-on:\s*ubuntu-26\.04\s*$", job, re.M), (
        f"{where} must run on the GitHub-hosted ubuntu-26.04 runner"
    )
    assert re.search(rf"^\s+needs:\s*{re.escape(needs)}\s*$", job, re.M), (
        f"{where} must need {needs}"
    )
    for forbidden in ("docker", "podman", "container:", "Containerfile"):
        assert forbidden not in job, f"{where} must not use {forbidden}"

    assert "DEBIAN_FRONTEND=noninteractive" in job, (
        f"{where} must install packages non-interactively"
    )
    assert re.search(r"postfix/main_mailer_type\s+select\s+No configuration",
                     job), f"{where} must preseed the postfix debconf"
    for package in REQUIRED_PACKAGES:
        assert re.search(rf"^\s+{re.escape(package)} \\$", job, re.M) or re.search(
            rf"^\s+{re.escape(package)}$", job, re.M
        ), f"{where} must install {package}"

    setups = meson_setup_commands(job)
    system = [s for s in setups if all(flag in s for flag in SYSTEM_FLAGS)]
    fixture = [s for s in setups if all(flag in s for flag in FIXTURE_FLAGS)]
    assert len(system) == 1, f"{where} must configure system Dovecot mode once"
    assert len(fixture) == 1, f"{where} must configure fixture Dovecot mode once"
    for setup in system + fixture:
        for flag in CAPNP_ENABLED_FLAGS:
            assert flag in setup, f"{where} setup must use {flag}: {setup}"

    assert job.count("meson compile") >= 2, f"{where} must build both modes"
    assert job.count("meson test") >= 2, f"{where} must test both modes"
    assert job.count("--print-errorlogs") >= 2, (
        f"{where} must print test error logs for both modes"
    )


def main() -> None:
    for path in REMOVED_PATHS:
        assert not path.exists(), (
            f"{path} must not exist; CI uses the GitHub-hosted runner image"
        )
    for workflow, needs in JOB_NEEDS.items():
        check_workflow_job(workflow, needs)


if __name__ == "__main__":
    main()
