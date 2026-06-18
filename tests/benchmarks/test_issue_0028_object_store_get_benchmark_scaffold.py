#!/usr/bin/env python3

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ROOT_MESON = REPO_ROOT / "meson.build"
BENCHMARK_MESON = REPO_ROOT / "tests" / "benchmarks" / "meson.build"
BENCHMARK_SOURCE = (
    REPO_ROOT
    / "tests"
    / "benchmarks"
    / "wyrebox-benchmark-object-store-get.c"
)


def main() -> None:
    root_text = ROOT_MESON.read_text(encoding="utf-8")
    benchmark_text = BENCHMARK_MESON.read_text(encoding="utf-8")
    source_text = BENCHMARK_SOURCE.read_text(encoding="utf-8")

    assert "subdir('tests/benchmarks')" in root_text
    assert "docs issue 0028 object store get benchmark scaffold" in root_text
    assert "benchmark(" in benchmark_text
    assert "object store get microbenchmark" in benchmark_text
    assert "wyrebox-benchmark-object-store-get.c" in benchmark_text
    assert "wyrebox_local_object_store_put_bytes" in source_text
    assert "wyrebox_local_object_store_get_bytes" in source_text
    assert "fixed RFC 5322 payload" in source_text
    assert "sha256:" in source_text
    assert "g_assert_cmpstr (object_key, ==, expected_key)" in source_text
    assert "g_file_test (object_path, G_FILE_TEST_IS_REGULAR)" in source_text
    assert "g_bytes_equal (payload, fetched)" in source_text
    assert 'g_print ("{\\"schema\\":\\"wyrebox-benchmark-report/v1\\",")' in (
        source_text
    )
    assert 'g_print ("\\"suite\\":\\"object-store\\",")' in source_text
    assert 'g_print ("\\"case\\":\\"get-bytes\\",")' in source_text
    assert 'g_print ("\\"metric\\":\\"elapsed_us\\",")' in source_text
    assert 'g_print ("\\"status\\":\\"ok\\"}\\n")' in source_text


if __name__ == "__main__":
    main()
