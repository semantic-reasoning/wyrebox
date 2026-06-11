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

daemon_runtime_test = executable(
  'test-daemon-runtime',
  'test-daemon-runtime.c',
  dependencies: [wyrebox_dep],
)

test('daemon success receipt', daemon_success_receipt_test)
test('daemon request identity', daemon_request_identity_test)
test('daemon runtime', daemon_runtime_test)
