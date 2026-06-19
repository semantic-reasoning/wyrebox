admin_socket_status_test = executable(
  'test-admin-socket-status',
  'test-admin-socket-status.c',
  dependencies: [wyrebox_dep],
)

test(
  'admin socket status',
  admin_socket_status_test,
  env: {
    'WYREBOX_ADMIN_EXECUTABLE': wyrebox_admin.full_path(),
  },
)
