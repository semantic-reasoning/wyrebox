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

