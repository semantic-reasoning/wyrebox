#!/usr/bin/env python3

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ROOT_MESON = REPO_ROOT / "meson.build"
BENCHMARK_MESON = REPO_ROOT / "tests" / "benchmarks" / "meson.build"
BENCHMARK_SOURCE = (
    REPO_ROOT / "tests" / "benchmarks" / "wyrebox-benchmark-journal-writer.c"
)


def main() -> None:
    root_text = ROOT_MESON.read_text(encoding="utf-8")
    benchmark_text = BENCHMARK_MESON.read_text(encoding="utf-8")
    source_text = BENCHMARK_SOURCE.read_text(encoding="utf-8")

    assert "subdir('tests/benchmarks')" in root_text
    assert "benchmark(" in benchmark_text
    assert "journal writer microbenchmark" in benchmark_text
    assert "wyrebox-benchmark-journal-writer.c" in benchmark_text
    assert "WYREBOX_JOURNAL_EVENT_DAEMON_AUDIT_RECORDED" in source_text
    assert "wyrebox_journal_writer_append" in source_text
    assert 'g_print ("{\\"schema\\":\\"wyrebox-benchmark-report/v1\\",")' in (
        source_text
    )
    assert 'g_print ("\\"metric\\":\\"elapsed_us\\",")' in source_text


if __name__ == "__main__":
    main()

