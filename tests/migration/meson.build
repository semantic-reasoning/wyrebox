schema_migration_test = executable(
  'test-schema-migration',
  'test-schema-migration.c',
  dependencies: [wyrebox_dep],
)

schema_metadata_store_test = executable(
  'test-schema-metadata-store',
  'test-schema-metadata-store.c',
  dependencies: [wyrebox_dep],
)

test(
  'schema migration',
  schema_migration_test,
)

test(
  'schema metadata store',
  schema_metadata_store_test,
)
