admin_socket_status_test = executable(
  'test-admin-socket-status',
  'test-admin-socket-status.c',
  dependencies: [wyrebox_dep],
)

admin_health_status_test = executable(
  'test-admin-health-status',
  'test-admin-health-status.c',
  dependencies: [wyrebox_dep],
)

admin_readiness_status_test = executable(
  'test-admin-readiness-status',
  'test-admin-readiness-status.c',
  dependencies: [wyrebox_dep],
)

test(
  'admin socket status',
  admin_socket_status_test,
  env: {
    'WYREBOX_ADMIN_EXECUTABLE': wyrebox_admin.full_path(),
  },
)

test(
  'admin health status',
  admin_health_status_test,
  env: {
    'WYREBOX_ADMIN_EXECUTABLE': wyrebox_admin.full_path(),
  },
)

test(
  'admin readiness status',
  admin_readiness_status_test,
  env: {
    'WYREBOX_ADMIN_EXECUTABLE': wyrebox_admin.full_path(),
  },
)
