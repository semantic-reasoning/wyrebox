wyrebox_sources = files(
  'wyrebox-local-object-store.c',
  'wyrebox-daemon-request-identity.c',
  'wyrebox-daemon-success-receipt.c',
  'wyrebox-delivery-projection.c',
  'wyrebox-delivery-replay-validator.c',
  'wyrebox-fact-record.c',
  'wyrebox-schema-metadata-store.c',
  'wyrebox-eml-ingestor.c',
  'wyrebox-eml-metadata.c',
  'wyrebox-message-delivered-payload.c',
  'wyrebox-journal-writer.c',
  'wyrebox-journal-reader.c',
  'wyrebox-schema-migration.c',
)

wyrebox_build_config = configuration_data()
wyrebox_build_config.set10('WYREBOX_HAVE_DUCKDB', duckdb_found)

configure_file(
  input: 'wyrebox-build-config.h.in',
  output: 'wyrebox-build-config.h',
  configuration: wyrebox_build_config,
)

wyrebox_inc = include_directories('.')

wyrebox_lib = static_library(
  'wyrebox',
  wyrebox_sources,
  include_directories: wyrebox_inc,
  dependencies: [glib_dep, gobject_dep, gio_dep],
)

wyrebox_dep = declare_dependency(
  include_directories: wyrebox_inc,
  link_with: wyrebox_lib,
  dependencies: [glib_dep, gobject_dep, gio_dep],
)
