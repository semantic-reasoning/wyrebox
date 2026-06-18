maildir_scanner_test = executable(
  'test-maildir-scanner',
  'test-maildir-scanner.c',
  dependencies: [wyrebox_dep],
)

test('maildir scanner', maildir_scanner_test, env: {
  'WYREBOX_MAILDIR_FIXTURE_DIR': meson.project_source_root() /
    'tests' / 'maildir' / 'fixtures' / 'root',
})
