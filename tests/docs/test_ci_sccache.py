#!/usr/bin/env python3

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS_DIR = REPO_ROOT / ".github" / "workflows"
WORKFLOWS = ["pr-lint.yml", "main-ci.yml"]
SCCACHE_ACTION = re.compile(
    r"^\s+uses:\s*mozilla-actions/sccache-action@v\d+\.\d+\.\d+\s*$", re.M
)


def read_workflow(workflow: str) -> tuple[str, dict[str, str]]:
    path = WORKFLOWS_DIR / workflow
    assert path.is_file(), f"missing workflow: {path}"
    text = path.read_text(encoding="utf-8")
    header, sep, jobs_text = text.partition("\njobs:\n")
    assert sep, f"{workflow}: missing top-level jobs key"

    jobs: dict[str, str] = {}
    matches = list(re.finditer(r"^  ([A-Za-z0-9_-]+):\n", jobs_text, re.M))
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else None
        jobs[match.group(1)] = jobs_text[match.end():end]
    return header, jobs


def assert_compiler_wrapped(where: str, job: str, variable: str) -> None:
    match = re.search(rf"^\s+{variable}:\s*\"?([^\"\n]+)\"?\s*$", job, re.M)
    assert match, f"{where} must set {variable}"
    assert match.group(1).startswith("sccache "), (
        f"{where} {variable} must run the compiler through sccache: "
        f"{match.group(1)}"
    )


def check_workflow(workflow: str) -> int:
    header, jobs = read_workflow(workflow)
    assert re.search(r"^\s+SCCACHE_GHA_ENABLED:\s*\"true\"\s*$", header, re.M), (
        f"{workflow} must enable the sccache GitHub Actions cache backend"
    )

    compiling = {name: job for name, job in jobs.items() if "meson compile" in job}
    assert compiling, f"{workflow} has no compiling jobs"
    for name, job in compiling.items():
        where = f"{workflow}:{name}"
        assert SCCACHE_ACTION.search(job), (
            f"{where} must set up mozilla-actions/sccache-action"
        )
        assert job.index("sccache-action") < job.index("meson setup"), (
            f"{where} must set up sccache before configuring"
        )
        assert_compiler_wrapped(where, job, "CC")
        assert_compiler_wrapped(where, job, "CXX")
        if "cmake -S" in job:
            for launcher in ("CMAKE_C_COMPILER_LAUNCHER",
                             "CMAKE_CXX_COMPILER_LAUNCHER"):
                assert f"-D{launcher}=sccache" in job, (
                    f"{where} must build DuckDB with {launcher}=sccache"
                )
    return len(compiling)


def main() -> None:
    checked = sum(check_workflow(workflow) for workflow in WORKFLOWS)
    print(f"sccache wiring checked for {checked} compiling jobs")


if __name__ == "__main__":
    main()
