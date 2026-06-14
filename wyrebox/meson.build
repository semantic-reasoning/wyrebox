wyrebox_sources = files(
  'object-store/wyrebox-local-object-store.c',
  'daemon/wyrebox-daemon-mailbox-list-request.c',
  'daemon/wyrebox-daemon-mailbox-list-result.c',
  'daemon/wyrebox-daemon-mailbox-list-service.c',
  'daemon/wyrebox-daemon-mailbox-catalog-duckdb.c',
  'daemon/wyrebox-daemon-mailbox-select-request.c',
  'daemon/wyrebox-daemon-mailbox-select-result.c',
  'daemon/wyrebox-daemon-mailbox-select-service.c',
  'daemon/wyrebox-daemon-client-identity.c',
  'daemon/wyrebox-daemon-connection-session.c',
  'daemon/wyrebox-daemon-mailbox-list-dispatcher.c',
  'daemon/wyrebox-daemon-mailbox-select-dispatcher.c',
  'daemon/wyrebox-daemon-request-router.c',
  'daemon/wyrebox-daemon-message-fetch-dispatcher.c',
  'daemon/wyrebox-daemon-message-fetch-request.c',
  'daemon/wyrebox-daemon-message-fetch-service.c',
  'daemon/wyrebox-daemon-message-search-dispatcher.c',
  'daemon/wyrebox-daemon-message-search-request.c',
  'daemon/wyrebox-daemon-message-search-service.c',
  'daemon/wyrebox-daemon-duckdb-query-template-dispatcher.c',
  'daemon/wyrebox-daemon-duckdb-query-template-catalog.c',
  'daemon/wyrebox-daemon-duckdb-query-template-request.c',
  'daemon/wyrebox-daemon-duckdb-query-template-service.c',
  'daemon/wyrebox-daemon-wirelog-predicate-query-dispatcher.c',
  'daemon/wyrebox-daemon-wirelog-predicate-query-catalog.c',
  'daemon/wyrebox-daemon-wirelog-predicate-query-request.c',
  'daemon/wyrebox-daemon-wirelog-predicate-query-service.c',
  'daemon/wyrebox-daemon-delivery-ingestion-dispatcher.c',
  'daemon/wyrebox-daemon-delivery-ingestion-service.c',
  'daemon/wyrebox-daemon-delivery-ingestion-request.c',
  'daemon/wyrebox-daemon-flag-keyword-update-dispatcher.c',
  'daemon/wyrebox-daemon-flag-keyword-update-request.c',
  'daemon/wyrebox-daemon-flag-keyword-update-service.c',
  'daemon/wyrebox-daemon-request-identity.c',
  'daemon/wyrebox-daemon-peer-credentials.c',
  'daemon/wyrebox-daemon-fact-batch-import-request.c',
  'daemon/wyrebox-daemon-fact-mutation-dispatcher.c',
  'daemon/wyrebox-daemon-fact-mutation-service.c',
  'daemon/wyrebox-daemon-fact-mutation-request.c',
  'daemon/wyrebox-daemon-connection-server.c',
  'daemon/wyrebox-daemon-frame-io.c',
  'daemon/wyrebox-daemon-uds-client.c',
  'daemon/wyrebox-daemon-request-adapter.c',
  'daemon/wyrebox-daemon-runtime.c',
  'daemon/wyrebox-daemon-socket-listener.c',
  'daemon/wyrebox-daemon-error.c',
  'daemon/wyrebox-daemon-error-frame.c',
  'daemon/wyrebox-daemon-response-frame.c',
  'daemon/wyrebox-daemon-stream-chunk-frame.c',
  'daemon/wyrebox-daemon-success-receipt.c',
  'dovecot/wyrebox-dovecot-daemon-client.c',
  'postfix/wyrebox-postfix-pipe-input.c',
  'postfix/wyrebox-postfix-pipe-outcome.c',
  'ingestion/wyrebox-delivery-materializer.c',
  'ingestion/wyrebox-delivery-fetcher.c',
  'ingestion/wyrebox-delivery-catchup.c',
  'ingestion/wyrebox-delivery-projection.c',
  'ingestion/wyrebox-delivery-replay-validator.c',
  'facts/wyrebox-deterministic-fact-extractor.c',
  'facts/wyrebox-fact-journal-snapshot.c',
  'facts/wyrebox-fact-reconciliation.c',
  'facts/wyrebox-fact-record.c',
  'schema/wyrebox-schema-metadata-store.c',
  'ingestion/wyrebox-eml-ingestor.c',
  'ingestion/wyrebox-eml-metadata.c',
  'journal/wyrebox-daemon-audit-payload.c',
  'journal/wyrebox-message-delivered-payload.c',
  'journal/wyrebox-derived-view-membership-changed-payload.c',
  'journal/wyrebox-journal-writer.c',
  'journal/wyrebox-journal-reader.c',
  'schema/wyrebox-schema-migration.c',
  'wirelog/wyrebox-derived-view-imap-name.c',
  'wirelog/wyrebox-derived-view-materializer.c',
  'wirelog/wyrebox-wirelog-rule-version.c',
)

wyrebox_build_config = configuration_data()
wyrebox_build_config.set10('WYREBOX_HAVE_DUCKDB', true)
wyrebox_build_config.set10('WYREBOX_DUCKDB_FROM_SUBPROJECT',
  duckdb_from_subproject)
wyrebox_build_config.set10('WYREBOX_HAVE_CAPNP_SERIALIZATION',
  capnp_serialization_found)
wyrebox_build_config.set10('WYREBOX_HAVE_DOVECOT_BACKEND',
  have_dovecot_backend)

configure_file(
  input: 'wyrebox-build-config.h.in',
  output: 'wyrebox-build-config.h',
  configuration: wyrebox_build_config,
)

wyrebox_inc = include_directories(
  '.',
  'object-store',
  'journal',
  'schema',
  'ingestion',
  'facts',
  'daemon',
  'dovecot',
  'postfix',
  'wirelog',
)

wyrebox_inc_paths = [wyrebox_inc]

wyrebox_link_deps = [
  glib_dep,
  gobject_dep,
  gio_dep,
  gio_unix_dep,
] + duckdb_deps

if capnp_serialization_found
  wyrebox_sources += [
    'daemon/wyrebox-daemon-capnp-codec.cc',
    daemon_api_capnp_schema,
  ]
  wyrebox_inc_paths += [daemon_api_capnp_schema_include]
  wyrebox_link_deps += [capnp_dep, kj_dep]
endif

if wirelog_runtime_found
  wyrebox_sources += [
    'daemon/wyrebox-daemon-wirelog-predicate-query-service-wirelog.c',
    'wirelog/wyrebox-derived-view-materializer-wirelog.c',
    'wirelog/wyrebox-wirelog-derived-membership.c',
    'wirelog/wyrebox-wirelog-evaluation.c',
    'wirelog/wyrebox-wirelog-program.c',
    'wirelog/wyrebox-wirelog-rule-definition.c',
    'wirelog/wyrebox-wirelog-source.c',
  ]
  wyrebox_link_deps += [wirelog_dep]
endif

wyrebox_lib = static_library(
  'wyrebox',
  wyrebox_sources,
  include_directories: wyrebox_inc_paths,
  dependencies: wyrebox_link_deps,
)

wyrebox_dep = declare_dependency(
  include_directories: wyrebox_inc_paths,
  link_with: wyrebox_lib,
  dependencies: wyrebox_link_deps,
)

wyrebox_dovecot_plugin_module = []
if have_dovecot_backend
  wyrebox_dovecot_plugin_module = shared_module(
    'wyrebox_plugin',
    'dovecot/wyrebox-dovecot-plugin.c',
    c_args: dovecot_backend_c_args + ['-fvisibility=hidden'],
    dependencies: [wyrebox_dep],
    link_args: ['-Wl,--exclude-libs,ALL'],
    name_prefix: '',
    install: false,
  )
endif

if capnp_serialization_found
  wyrebox_postfix_pipe = executable(
    'wyrebox-postfix-pipe',
    'postfix/wyrebox-postfix-pipe-main.c',
    dependencies: [wyrebox_dep],
    install: true,
  )
endif
