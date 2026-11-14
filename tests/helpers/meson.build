wyrebox_test_daemon_lib = static_library(
  'wyrebox-test-daemon',
  'wyrebox-test-daemon.c',
  dependencies: [dependency('gio-unix-2.0'), dependency('glib-2.0')],
  include_directories: include_directories('.'),
)

wyrebox_test_daemon_dep = declare_dependency(
  link_with: wyrebox_test_daemon_lib,
  include_directories: include_directories('.'),
  dependencies: [dependency('gio-unix-2.0'), dependency('glib-2.0')],
)
