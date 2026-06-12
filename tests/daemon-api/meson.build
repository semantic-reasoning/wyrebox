daemon_success_receipt_test = executable(
  'test-daemon-success-receipt',
  'test-daemon-success-receipt.c',
  dependencies: [wyrebox_dep],
)

daemon_request_identity_test = executable(
  'test-daemon-request-identity',
  'test-daemon-request-identity.c',
  dependencies: [wyrebox_dep],
)

daemon_peer_credentials_test = executable(
  'test-daemon-peer-credentials',
  'test-daemon-peer-credentials.c',
  dependencies: [wyrebox_dep],
)

daemon_mailbox_list_request_test = executable(
  'test-daemon-mailbox-list-request',
  'test-daemon-mailbox-list-request.c',
  dependencies: [wyrebox_dep],
)

daemon_mailbox_list_result_test = executable(
  'test-daemon-mailbox-list-result',
  'test-daemon-mailbox-list-result.c',
  dependencies: [wyrebox_dep],
)

daemon_mailbox_select_request_test = executable(
  'test-daemon-mailbox-select-request',
  'test-daemon-mailbox-select-request.c',
  dependencies: [wyrebox_dep],
)

daemon_mailbox_select_result_test = executable(
  'test-daemon-mailbox-select-result',
  'test-daemon-mailbox-select-result.c',
  dependencies: [wyrebox_dep],
)

daemon_request_router_test = executable(
  'test-daemon-request-router',
  'test-daemon-request-router.c',
  dependencies: [wyrebox_dep],
)

daemon_fact_mutation_request_test = executable(
  'test-daemon-fact-mutation-request',
  'test-daemon-fact-mutation-request.c',
  dependencies: [wyrebox_dep],
)

daemon_fact_mutation_service_test = executable(
  'test-daemon-fact-mutation-service',
  'test-daemon-fact-mutation-service.c',
  dependencies: [wyrebox_dep],
)

daemon_fact_mutation_dispatcher_test = executable(
  'test-daemon-fact-mutation-dispatcher',
  'test-daemon-fact-mutation-dispatcher.c',
  dependencies: [wyrebox_dep],
)

daemon_mailbox_select_dispatcher_test = executable(
  'test-daemon-mailbox-select-dispatcher',
  'test-daemon-mailbox-select-dispatcher.c',
  dependencies: [wyrebox_dep],
)

daemon_runtime_test = executable(
  'test-daemon-runtime',
  'test-daemon-runtime.c',
  dependencies: [wyrebox_dep],
)

daemon_socket_listener_test = executable(
  'test-daemon-socket-listener',
  'test-daemon-socket-listener.c',
  dependencies: [wyrebox_dep],
)

daemon_error_test = executable(
  'test-daemon-error',
  'test-daemon-error.c',
  dependencies: [wyrebox_dep],
)

daemon_error_frame_test = executable(
  'test-daemon-error-frame',
  'test-daemon-error-frame.c',
  dependencies: [wyrebox_dep],
)

daemon_response_frame_test = executable(
  'test-daemon-response-frame',
  'test-daemon-response-frame.c',
  dependencies: [wyrebox_dep],
)

daemon_connection_session_test = executable(
  'test-daemon-connection-session',
  'test-daemon-connection-session.c',
  dependencies: [wyrebox_dep],
)

daemon_connection_server_test = executable(
  'test-daemon-connection-server',
  'test-daemon-connection-server.c',
  dependencies: [wyrebox_dep],
)

daemon_request_adapter_test = executable(
  'test-daemon-request-adapter',
  'test-daemon-request-adapter.c',
  dependencies: [wyrebox_dep],
)

daemon_frame_io_test = executable(
  'test-daemon-frame-io',
  'test-daemon-frame-io.c',
  dependencies: [wyrebox_dep],
)

if capnp_serialization_found
  daemon_api_capnp_smoke_test = executable(
    'test-daemon-api-capnp-smoke',
    [
      'test-daemon-api-capnp-smoke.cc',
      daemon_api_capnp_schema,
    ],
    dependencies: [capnp_dep, kj_dep],
    include_directories: daemon_api_capnp_schema_include,
  )

  daemon_api_capnp_codec_test = executable(
    'test-daemon-capnp-codec',
    [
      'test-daemon-capnp-codec.cc',
      daemon_api_capnp_schema,
    ],
    dependencies: [wyrebox_dep, capnp_dep, kj_dep],
    include_directories: [daemon_api_capnp_schema_include],
  )
endif

test('daemon success receipt', daemon_success_receipt_test)
test('daemon request identity', daemon_request_identity_test)
test('daemon peer credentials', daemon_peer_credentials_test)
test('daemon mailbox list request', daemon_mailbox_list_request_test)
test('daemon mailbox list result', daemon_mailbox_list_result_test)
test('daemon mailbox select request', daemon_mailbox_select_request_test)
test('daemon mailbox select result', daemon_mailbox_select_result_test)
test('daemon request router', daemon_request_router_test)
test('daemon fact mutation request', daemon_fact_mutation_request_test)
test('daemon fact mutation service', daemon_fact_mutation_service_test)
test('daemon fact mutation dispatcher', daemon_fact_mutation_dispatcher_test)
test('daemon mailbox select dispatcher', daemon_mailbox_select_dispatcher_test)
test('daemon runtime', daemon_runtime_test)
test('daemon socket listener', daemon_socket_listener_test)
test('daemon error', daemon_error_test)
test('daemon error frame', daemon_error_frame_test)
test('daemon response frame', daemon_response_frame_test)
test('daemon connection session', daemon_connection_session_test)
test('daemon connection server', daemon_connection_server_test)
test('daemon request adapter', daemon_request_adapter_test)
test('daemon frame io', daemon_frame_io_test)
if capnp_serialization_found
  test('daemon api capnp smoke', daemon_api_capnp_smoke_test)
  test('daemon capnp codec', daemon_api_capnp_codec_test)
endif
