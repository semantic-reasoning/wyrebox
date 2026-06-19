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

test('backup manifest', backup_manifest_test)
test('backup restore plan', backup_restore_plan_test)
