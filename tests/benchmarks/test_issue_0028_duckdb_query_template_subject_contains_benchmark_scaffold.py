#!/usr/bin/env python3

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ROOT_MESON = REPO_ROOT / "meson.build"
BENCHMARK_MESON = REPO_ROOT / "tests" / "benchmarks" / "meson.build"
BENCHMARK_SOURCE = (
    REPO_ROOT
    / "tests"
    / "benchmarks"
    / "wyrebox-benchmark-duckdb-query-template-subject-contains.c"
)
BENCHMARK_REPORT = (
    REPO_ROOT / "tests" / "benchmarks" / "wyrebox-benchmark-report.c"
)


def main() -> None:
    root_text = ROOT_MESON.read_text(encoding="utf-8")
    benchmark_text = BENCHMARK_MESON.read_text(encoding="utf-8")
    source_text = BENCHMARK_SOURCE.read_text(encoding="utf-8")
    report_text = BENCHMARK_REPORT.read_text(encoding="utf-8")

    assert "subdir('tests/benchmarks')" in root_text
    assert (
        "docs issue 0028 duckdb query template subject contains benchmark scaffold"
        in root_text
    )
    assert "benchmark(" in benchmark_text
    assert "duckdb query template subject contains microbenchmark" in benchmark_text
    assert "wyrebox-benchmark-duckdb-query-template-subject-contains.c" in (
        benchmark_text
    )
    assert "wyrebox_daemon_duckdb_query_template_service_new_duckdb" in (
        source_text
    )
    assert "messages.subject_contains.v1" in source_text
    assert 'init_messages_subject_contains_request (&request, "account-1",' in (
        source_text
    )
    assert "count_substring (csv, \"object-volume-\")" in source_text
    assert "message-volume-095" in source_text
    assert "message-volume-104" in source_text
    assert "message-cross-account" in source_text
    assert "message-nonmatching" in source_text
    assert "message-null-subject" in source_text
    assert "wyrebox_benchmark_report_print_json" in source_text
    assert "wyrebox_benchmark_report_capture_rusage" in source_text
    assert '"duckdb-query-template"' in source_text
    assert '"messages-subject-contains"' in source_text
    assert "cpu_user_us" in report_text
    assert "cpu_system_us" in report_text
    assert "max_rss_kb" in report_text
    assert "object_count" in report_text
    assert "disk_bytes" in report_text


if __name__ == "__main__":
    main()
