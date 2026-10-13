#!/usr/bin/env python3

import os
import re
import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "ci-core-backtraces.sh"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "pr-lint.yml"
JOBS = ["pr-build", "pr-build-minimal"]

FAKE_GDB = """#!/bin/sh
printf '%s\\n' "$@" >> "$GDB_LOG"
printf -- '---\\n' >> "$GDB_LOG"
"""


def read_jobs() -> dict[str, str]:
    text = WORKFLOW.read_text(encoding="utf-8")
    _, sep, jobs_text = text.partition("\njobs:\n")
    assert sep, "pr-lint.yml: missing top-level jobs key"
    jobs: dict[str, str] = {}
    matches = list(re.finditer(r"^  ([A-Za-z0-9_-]+):\n", jobs_text, re.M))
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else None
        jobs[match.group(1)] = jobs_text[match.end():end]
    return jobs


def split_steps(job: str) -> list[str]:
    starts = [m.start() for m in re.finditer(r"^      - ", job, re.M)]
    return [job[s:e] for s, e in zip(starts, starts[1:] + [len(job)])]


def check_workflow() -> None:
    jobs = read_jobs()
    for name in JOBS:
        assert name in jobs, f"pr-lint.yml: missing job {name}"
        steps = split_steps(jobs[name])
        test_steps = [s for s in steps if "meson test" in s]
        assert len(test_steps) == 1, f"{name}: expected one meson test step"
        test = test_steps[0]
        enable = test.find("tools/ci-core-backtraces.sh enable")
        ulimit = test.find("ulimit -c unlimited")
        run = test.find("meson test")
        assert enable != -1, f"{name}: test step must enable core dumps"
        assert ulimit != -1, (
            f"{name}: test step must raise the core size limit in the "
            "same shell as meson test"
        )
        assert enable < run and ulimit < run, (
            f"{name}: core dumps must be enabled before meson test"
        )
        for line in test.splitlines():
            if "ci-core-backtraces.sh enable" in line or "ulimit" in line:
                assert "||" in line, (
                    f"{name}: diagnostics setup must not abort the tests: "
                    f"{line.strip()}"
                )

        reports = [s for s in steps
                   if "tools/ci-core-backtraces.sh report" in s]
        assert len(reports) == 1, f"{name}: expected one backtrace step"
        report = reports[0]
        assert steps.index(report) > steps.index(test), (
            f"{name}: backtrace step must follow the test step"
        )
        assert re.search(r"^\s+if:\s*failure\(\)\s*$", report, re.M), (
            f"{name}: backtrace step must run only on failure"
        )
        assert re.search(r"^\s+continue-on-error:\s*true\s*$", report,
                         re.M), (
            f"{name}: backtrace step must not mask the test failure"
        )
        installs = [line for line in report.splitlines()
                    if "apt-get install" in line and "gdb" in line]
        assert len(installs) == 1, f"{name}: backtrace step must install gdb"
        assert "|| true" in installs[0], (
            f"{name}: a failed gdb install must still list the cores"
        )


def run_report(cores: Path, log: Path, bindir: Path) -> str:
    env = dict(os.environ)
    env["PATH"] = f"{bindir}{os.pathsep}{env['PATH']}"
    env["GDB_LOG"] = str(log)
    result = subprocess.run(
        ["bash", str(SCRIPT), "report", str(cores)],
        env=env, capture_output=True, text=True, check=False,
    )
    assert result.returncode == 0, (
        f"report must not fail the job: {result.returncode}\n"
        f"{result.stdout}{result.stderr}"
    )
    return result.stdout


def check_report() -> None:
    assert SCRIPT.is_file(), f"missing {SCRIPT}"
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        bindir = root / "bin"
        bindir.mkdir()
        gdb = bindir / "gdb"
        gdb.write_text(FAKE_GDB, encoding="utf-8")
        gdb.chmod(0o755)
        log = root / "gdb.log"
        cores = root / "cores"

        out = run_report(cores, log, bindir)
        assert "no core files" in out, f"missing directory output: {out}"
        assert not log.exists(), "gdb must not run without cores"

        cores.mkdir()
        out = run_report(cores, log, bindir)
        assert "no core files" in out, f"empty directory output: {out}"
        assert not log.exists(), "gdb must not run without cores"

        exe = root / "build dir" / "test-delivery-catchup"
        exe.parent.mkdir()
        exe.write_text("", encoding="utf-8")
        mangled = str(exe).replace("/", "!")
        known = cores / f"core.{mangled}.4242"
        known.write_text("", encoding="utf-8")
        unknown = cores / "core.!nonexistent!binary.7"
        unknown.write_text("", encoding="utf-8")

        out = run_report(cores, log, bindir)
        calls = [c.strip().splitlines()
                 for c in log.read_text(encoding="utf-8").split("---")
                 if c.strip()]
        assert len(calls) == 1, f"expected one gdb run, got {calls}"
        args = calls[0]
        assert args[-2:] == [str(exe), str(known)], (
            f"gdb must load the core against its executable: {args}"
        )
        assert "-batch" in args, f"gdb must run in batch mode: {args}"
        assert "thread apply all bt" in args, f"missing backtrace: {args}"
        assert str(unknown) in out, (
            f"unresolved cores must be named in the output: {out}"
        )


def main() -> None:
    check_workflow()
    check_report()
    print("core dump backtraces checked for", ", ".join(JOBS))


if __name__ == "__main__":
    main()
