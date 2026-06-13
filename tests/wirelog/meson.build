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

test('wirelog link smoke', wirelog_link_smoke_test)
test('wirelog program', wirelog_program_test)
test('wirelog source', wirelog_source_test)
