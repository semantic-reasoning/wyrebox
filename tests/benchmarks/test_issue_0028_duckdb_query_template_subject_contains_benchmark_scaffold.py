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


def main() -> None:
    root_text = ROOT_MESON.read_text(encoding="utf-8")
    benchmark_text = BENCHMARK_MESON.read_text(encoding="utf-8")
    source_text = BENCHMARK_SOURCE.read_text(encoding="utf-8")

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
    assert 'g_print ("{\\"schema\\":\\"wyrebox-benchmark-report/v1\\",")' in (
        source_text
    )
    assert 'g_print ("\\"suite\\":\\"duckdb-query-template\\",")' in source_text
    assert 'g_print ("\\"case\\":\\"messages-subject-contains\\",")' in source_text
    assert 'g_print ("\\"metric\\":\\"elapsed_us\\",")' in source_text
    assert 'g_print ("\\"status\\":\\"ok\\"}\\n")' in source_text


if __name__ == "__main__":
    main()
