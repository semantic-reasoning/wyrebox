#!/usr/bin/env python3

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ROOT_MESON = REPO_ROOT / "meson.build"
BENCHMARK_MESON = REPO_ROOT / "tests" / "benchmarks" / "meson.build"
BENCHMARK_SOURCE = (
    REPO_ROOT / "tests" / "benchmarks" / "wyrebox-benchmark-object-store.c"
)


def main() -> None:
    root_text = ROOT_MESON.read_text(encoding="utf-8")
    benchmark_text = BENCHMARK_MESON.read_text(encoding="utf-8")
    source_text = BENCHMARK_SOURCE.read_text(encoding="utf-8")

    assert "subdir('tests/benchmarks')" in root_text
    assert "benchmark(" in benchmark_text
    assert "object store put microbenchmark" in benchmark_text
    assert "wyrebox-benchmark-object-store.c" in benchmark_text
    assert "WYREBOX_BENCHMARK_FIXTURE_DIR" in benchmark_text
    assert "simple-crlf.eml" in source_text
    assert "wyrebox_local_object_store_put_bytes" in source_text
    assert "wyrebox_local_object_store_get_bytes" in source_text
    assert 'g_print ("{\\"schema\\":\\"wyrebox-benchmark-report/v1\\",")' in (
        source_text
    )
    assert 'g_print ("\\"metric\\":\\"elapsed_us\\",")' in source_text
    assert '\\"suite\\":\\"object-store\\"' in source_text
    assert '\\"case\\":\\"put-bytes\\"' in source_text


if __name__ == "__main__":
    main()
