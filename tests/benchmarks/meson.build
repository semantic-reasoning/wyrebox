benchmark_fixture_dir = meson.project_source_root() / 'tests' / 'object-store' / 'fixtures'

eml_ingestor_microbenchmark = executable(
  'wyrebox-benchmark-eml-ingestor',
  'wyrebox-benchmark-eml-ingestor.c',
  dependencies: [wyrebox_dep],
)

benchmark(
  'eml ingestor microbenchmark',
  eml_ingestor_microbenchmark,
  env: {
    'WYREBOX_BENCHMARK_FIXTURE_DIR': benchmark_fixture_dir,
  },
)

journal_writer_microbenchmark = executable(
  'wyrebox-benchmark-journal-writer',
  'wyrebox-benchmark-journal-writer.c',
  dependencies: [wyrebox_dep],
)

benchmark(
  'journal writer microbenchmark',
  journal_writer_microbenchmark,
)

object_store_benchmark = executable(
  'wyrebox-benchmark-object-store',
  'wyrebox-benchmark-object-store.c',
  dependencies: [wyrebox_dep],
)

benchmark(
  'object store put microbenchmark',
  object_store_benchmark,
  env: {
    'WYREBOX_BENCHMARK_FIXTURE_DIR': benchmark_fixture_dir,
  },
)

object_store_get_benchmark = executable(
  'wyrebox-benchmark-object-store-get',
  'wyrebox-benchmark-object-store-get.c',
  dependencies: [wyrebox_dep],
)

benchmark(
  'object store get microbenchmark',
  object_store_get_benchmark,
)

duckdb_query_template_subject_contains_benchmark = executable(
  'wyrebox-benchmark-duckdb-query-template-subject-contains',
  'wyrebox-benchmark-duckdb-query-template-subject-contains.c',
  dependencies: [wyrebox_dep],
)

benchmark(
  'duckdb query template subject contains microbenchmark',
  duckdb_query_template_subject_contains_benchmark,
)
