object_store_test = executable(
  'test-local-object-store',
  'test-local-object-store.c',
  dependencies: [wyrebox_dep],
)

test('local object store', object_store_test)

eml_byte_preservation_test = executable(
  'test-eml-byte-preservation',
  'test-eml-byte-preservation.c',
  dependencies: [wyrebox_dep],
)

test(
  'eml byte preservation',
  eml_byte_preservation_test,
  env: {
    'WYREBOX_EML_FIXTURE_DIR': meson.current_source_dir() / 'fixtures',
  },
)
