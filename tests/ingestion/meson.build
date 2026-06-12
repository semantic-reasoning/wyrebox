eml_fixture_dir = meson.project_source_root()
eml_fixture_dir = eml_fixture_dir / 'tests' / 'object-store' / 'fixtures'

eml_ingestor_test = executable(
  'test-eml-ingestor',
  'test-eml-ingestor.c',
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

delivery_materializer_test = executable(
  'test-delivery-materializer',
  'test-delivery-materializer.c',
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

test(
  'delivery materializer',
  delivery_materializer_test,
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
