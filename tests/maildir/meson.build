maildir_scanner_test = executable(
  'test-maildir-scanner',
  'test-maildir-scanner.c',
  dependencies: [wyrebox_dep],
)

maildir_import_plan_test = executable(
  'test-maildir-import-plan',
  'test-maildir-import-plan.c',
  dependencies: [wyrebox_dep],
)

test('maildir scanner', maildir_scanner_test, env: {
  'WYREBOX_MAILDIR_FIXTURE_DIR': meson.project_source_root() /
    'tests' / 'maildir' / 'fixtures' / 'root',
})

test('maildir import plan', maildir_import_plan_test, env: {
  'WYREBOX_MAILDIR_FIXTURE_DIR': meson.project_source_root() /
    'tests' / 'maildir' / 'fixtures' / 'root',
})
