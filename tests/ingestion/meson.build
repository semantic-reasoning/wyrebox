eml_fixture_dir = meson.project_source_root()
eml_fixture_dir = eml_fixture_dir / 'tests' / 'object-store' / 'fixtures'

eml_ingestor_test = executable(
  'test-eml-ingestor',
  'test-eml-ingestor.c',
  dependencies: [wyrebox_dep],
)

eml_ingestion_roundtrip_test = executable(
  'test-eml-ingestion-roundtrip',
  'test-eml-ingestion-roundtrip.c',
  dependencies: [wyrebox_dep],
)

eml_duplicate_behavior_test = executable(
  'test-eml-duplicate-behavior',
  'test-eml-duplicate-behavior.c',
  dependencies: [wyrebox_dep],
)

delivery_replay_validator_test = executable(
  'test-delivery-replay-validator',
  'test-delivery-replay-validator.c',
  dependencies: [wyrebox_dep],
)

delivery_projection_test = executable(
  'test-delivery-projection',
  'test-delivery-projection.c',
  dependencies: [wyrebox_dep],
)

rfc5322_date_test = executable(
  'test-rfc5322-date',
  'test-rfc5322-date.c',
  dependencies: [wyrebox_dep],
)

delivery_materializer_test = executable(
  'test-delivery-materializer',
  'test-delivery-materializer.c',
  dependencies: [wyrebox_dep],
)

delivery_fetcher_test = executable(
  'test-delivery-fetcher',
  'test-delivery-fetcher.c',
  dependencies: [wyrebox_dep],
)

delivery_catchup_test = executable(
  'test-delivery-catchup',
  'test-delivery-catchup.c',
  dependencies: [wyrebox_dep],
)

test(
  'eml ingestor',
  eml_ingestor_test,
  env: {
    'WYREBOX_EML_FIXTURE_DIR': eml_fixture_dir,
  },
)

test(
  'eml ingestion roundtrip',
  eml_ingestion_roundtrip_test,
  env: {
    'WYREBOX_EML_FIXTURE_DIR': eml_fixture_dir,
  },
)

test('duplicate delivery behavior', eml_duplicate_behavior_test)

test(
  'delivery replay validator',
  delivery_replay_validator_test,
  env: {
    'WYREBOX_EML_FIXTURE_DIR': eml_fixture_dir,
  },
)

test(
  'delivery projection',
  delivery_projection_test,
  env: {
    'WYREBOX_EML_FIXTURE_DIR': eml_fixture_dir,
  },
)

test('rfc5322 date parser', rfc5322_date_test)

test(
  'delivery materializer',
  delivery_materializer_test,
)

test(
  'delivery fetcher',
  delivery_fetcher_test,
  env: {
    'WYREBOX_EML_FIXTURE_DIR': eml_fixture_dir,
  },
)

test(
  'delivery catchup',
  delivery_catchup_test,
  env: {
    'WYREBOX_EML_FIXTURE_DIR': eml_fixture_dir,
  },
)

eml_metadata_test = executable(
  'test-eml-metadata',
  'test-eml-metadata.c',
  dependencies: [wyrebox_dep],
)

test(
  'eml metadata',
  eml_metadata_test,
  env: {
    'WYREBOX_EML_FIXTURE_DIR': eml_fixture_dir,
  },
)
