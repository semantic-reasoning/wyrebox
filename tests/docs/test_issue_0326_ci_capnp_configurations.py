#!/usr/bin/env python3

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"

CAPNP_ENABLED_FLAGS = ["-Dcapnp=enabled", "-Dcapnp_serialization=enabled"]
CAPNP_DISABLED_FLAGS = ["-Dcapnp=disabled", "-Dcapnp_serialization=disabled"]
CAPNP_PACKAGES = ["capnproto", "libcapnp-dev"]

CAPNP_JOBS = {
    "pr-lint.yml": ["pr-build", "pr-sanitize"],
    "main-ci.yml": ["build", "sanitize"],
}
MINIMAL_JOB = ("pr-lint.yml", "pr-build-minimal")


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


def meson_setup_commands(job_text: str) -> list[str]:
    # Join backslash continuations so each setup command is one string.
    joined = re.sub(r"\\\n\s*", " ", job_text)
    return [line for line in joined.splitlines() if "meson setup" in line]


def assert_capnp_job(workflow: str, name: str, job_text: str) -> None:
    for package in CAPNP_PACKAGES:
        assert re.search(rf"^\s+{re.escape(package)} \\$", job_text, re.M), (
            f"{workflow}:{name} must install {package}"
        )

    setups = meson_setup_commands(job_text)
    assert setups, f"{workflow}:{name} has no meson setup command"
    for setup in setups:
        for flag in CAPNP_ENABLED_FLAGS:
            assert flag in setup, (
                f"{workflow}:{name} must configure with {flag} so a missing "
                f"Cap'n Proto toolchain fails meson setup: {setup.strip()}"
            )


def assert_minimal_job(workflow: str, name: str, job_text: str) -> None:
    for package in CAPNP_PACKAGES:
        assert package not in job_text, (
            f"{workflow}:{name} must not install {package}"
        )

    setups = meson_setup_commands(job_text)
    assert setups, f"{workflow}:{name} has no meson setup command"
    for setup in setups:
        for flag in CAPNP_DISABLED_FLAGS:
            assert flag in setup, (
                f"{workflow}:{name} must configure with {flag}: "
                f"{setup.strip()}"
            )

    assert "meson compile" in job_text, f"{workflow}:{name} must build"
    assert "meson test" in job_text, f"{workflow}:{name} must run tests"


def main() -> None:
    for workflow, names in CAPNP_JOBS.items():
        jobs = read_jobs(workflow)
        for name in names:
            assert name in jobs, f"{workflow}: missing job {name}"
            assert_capnp_job(workflow, name, jobs[name])

    workflow, name = MINIMAL_JOB
    jobs = read_jobs(workflow)
    assert name in jobs, f"{workflow}: missing job {name}"
    assert_minimal_job(workflow, name, jobs[name])


if __name__ == "__main__":
    main()
