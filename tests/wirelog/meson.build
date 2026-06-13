wirelog_rule_version_test = executable(
  'test-wirelog-rule-version',
  'test-wirelog-rule-version.c',
  dependencies: [wyrebox_dep],
)

derived_view_materializer_test = executable(
  'test-derived-view-materializer',
  'test-derived-view-materializer.c',
  dependencies: [wyrebox_dep],
)

test('wirelog rule version', wirelog_rule_version_test)
test('derived view materializer', derived_view_materializer_test)

if wirelog_runtime_found
  wirelog_link_smoke_test = executable(
    'test-wirelog-link-smoke',
    'test-wirelog-link-smoke.c',
    dependencies: [
      glib_dep,
      wirelog_dep,
    ],
  )

  wirelog_program_test = executable(
    'test-wirelog-program',
    'test-wirelog-program.c',
    dependencies: [wyrebox_dep],
  )

  wirelog_source_test = executable(
    'test-wirelog-source',
    'test-wirelog-source.c',
    dependencies: [wyrebox_dep],
  )

  wirelog_evaluation_test = executable(
    'test-wirelog-evaluation',
    'test-wirelog-evaluation.c',
    dependencies: [wyrebox_dep],
  )

  wirelog_derived_membership_test = executable(
    'test-wirelog-derived-membership',
    'test-wirelog-derived-membership.c',
    dependencies: [wyrebox_dep],
  )

  derived_view_materializer_wirelog_test = executable(
    'test-derived-view-materializer-wirelog',
    'test-derived-view-materializer-wirelog.c',
    dependencies: [wyrebox_dep],
  )

  test('wirelog link smoke', wirelog_link_smoke_test)
  test('wirelog program', wirelog_program_test)
  test('wirelog source', wirelog_source_test)
  test('wirelog evaluation', wirelog_evaluation_test)
  test('wirelog derived membership', wirelog_derived_membership_test)
  test('derived view materializer wirelog',
    derived_view_materializer_wirelog_test)
endif
