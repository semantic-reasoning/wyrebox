postfix_pipe_input_test = executable(
  'test-postfix-pipe-input',
  'test-postfix-pipe-input.c',
  dependencies: [wyrebox_dep],
)

postfix_pipe_outcome_test = executable(
  'test-postfix-pipe-outcome',
  'test-postfix-pipe-outcome.c',
  dependencies: [wyrebox_dep],
)

if capnp_serialization_found
  postfix_pipe_executable_test = executable(
    'test-postfix-pipe-executable',
    [
      'test-postfix-pipe-executable.cc',
      daemon_api_capnp_schema,
    ],
    dependencies: [wyrebox_dep, capnp_dep, kj_dep],
    include_directories: [daemon_api_capnp_schema_include],
  )
endif

test('postfix pipe input', postfix_pipe_input_test)
test('postfix pipe outcome', postfix_pipe_outcome_test)
if capnp_serialization_found
  test(
    'postfix pipe executable',
    postfix_pipe_executable_test,
    env: {
      'WYREBOX_POSTFIX_PIPE_EXECUTABLE': wyrebox_postfix_pipe.full_path(),
    },
  )
endif
