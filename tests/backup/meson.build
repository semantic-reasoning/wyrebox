backup_manifest_test = executable(
  'test-backup-manifest',
  'test-backup-manifest.c',
  dependencies: [wyrebox_dep],
)

backup_restore_plan_test = executable(
  'test-backup-restore-plan',
  'test-backup-restore-plan.c',
  dependencies: [wyrebox_dep],
)

backup_set_test = executable(
  'test-backup-set',
  'test-backup-set.c',
  dependencies: [wyrebox_dep],
)

backup_command_test = executable(
  'test-backup-command',
  'test-backup-command.c',
  dependencies: [wyrebox_dep],
)

backup_tool_main_test = executable(
  'test-backup-tool-main',
  'test-backup-tool-main.c',
  dependencies: [wyrebox_dep],
)

test('backup manifest', backup_manifest_test)
test('backup restore plan', backup_restore_plan_test)
test('backup set', backup_set_test)
test('backup command', backup_command_test)
test('backup tool main', backup_tool_main_test)
