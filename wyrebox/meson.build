wyrebox_sources = files(
  'wyrebox-local-object-store.c',
  'wyrebox-daemon-mailbox-list-request.c',
  'wyrebox-daemon-mailbox-list-result.c',
  'wyrebox-daemon-mailbox-list-service.c',
  'wyrebox-daemon-mailbox-list-dispatcher.c',
  'wyrebox-daemon-request-router.c',
  'wyrebox-daemon-request-identity.c',
  'wyrebox-daemon-fact-mutation-dispatcher.c',
  'wyrebox-daemon-fact-mutation-service.c',
  'wyrebox-daemon-fact-mutation-request.c',
  'wyrebox-daemon-runtime.c',
  'wyrebox-daemon-error.c',
  'wyrebox-daemon-error-frame.c',
  'wyrebox-daemon-response-frame.c',
  'wyrebox-daemon-success-receipt.c',
  'wyrebox-delivery-materializer.c',
  'wyrebox-delivery-catchup.c',
  'wyrebox-delivery-projection.c',
  'wyrebox-delivery-replay-validator.c',
  'wyrebox-deterministic-fact-extractor.c',
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
wyrebox_build_config.set10('WYREBOX_HAVE_DUCKDB', true)
wyrebox_build_config.set10('WYREBOX_HAVE_CAPNP_SERIALIZATION',
  capnp_serialization_found)

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
  dependencies: [glib_dep, gobject_dep, gio_dep, duckdb_deps],
)

wyrebox_dep = declare_dependency(
  include_directories: wyrebox_inc,
  link_with: wyrebox_lib,
  dependencies: [glib_dep, gobject_dep, gio_dep, duckdb_deps],
)
