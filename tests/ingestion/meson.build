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
