postfix_pipe_input_test = executable ('test-postfix-pipe-input', 'test-postfix-pipe-input.c', dependencies:[wyrebox_dep],)
postfix_pipe_outcome_test = executable ('test-postfix-pipe-outcome', 'test-postfix-pipe-outcome.c', dependencies:[wyrebox_dep],)
postfix_lmtp_session_test = executable ('test-postfix-lmtp-session', 'test-postfix-lmtp-session.c', dependencies:[wyrebox_dep],)
postfix_lmtp_reply_test = executable ('test-postfix-lmtp-reply', 'test-postfix-lmtp-reply.c', dependencies:[wyrebox_dep],)
    if capnp_serialization_found
postfix_pipe_executable_test = executable ('test-postfix-pipe-executable',['test-postfix-pipe-executable.cc', daemon_api_capnp_schema,], dependencies: [wyrebox_dep, capnp_dep, kj_dep], include_directories:[daemon_api_capnp_schema_include],)
postfix_lmtp_delivery_bridge_test = executable ('test-postfix-lmtp-delivery-bridge',['test-postfix-lmtp-delivery-bridge.cc', daemon_api_capnp_schema,], dependencies: [wyrebox_dep, capnp_dep, kj_dep], include_directories:[daemon_api_capnp_schema_include],)
postfix_lmtp_executable_test = executable ('test-postfix-lmtp-executable',['test-postfix-lmtp-executable.cc', daemon_api_capnp_schema,], dependencies: [wyrebox_dep, capnp_dep, kj_dep], include_directories:[daemon_api_capnp_schema_include],)
postfix_lmtp_delivery_test = executable ('test-postfix-lmtp-delivery',['test-postfix-lmtp-delivery.cc', daemon_api_capnp_schema,], dependencies: [wyrebox_dep, capnp_dep, kj_dep], include_directories:[daemon_api_capnp_schema_include],)
postfix_pipe_real_daemon_test = executable ('test-postfix-pipe-real-daemon',['test-postfix-pipe-real-daemon.c', daemon_api_capnp_schema,], dependencies: [wyrebox_dep, capnp_dep, kj_dep], include_directories:[daemon_api_capnp_schema_include],)
     endif
     test ('postfix pipe input', postfix_pipe_input_test)
test ('postfix pipe outcome', postfix_pipe_outcome_test)
test ('postfix lmtp session', postfix_lmtp_session_test)
test ('postfix lmtp reply', postfix_lmtp_reply_test)
if capnp_serialization_found
test ('postfix pipe executable', postfix_pipe_executable_test, env:
    {
'WYREBOX_POSTFIX_PIPE_EXECUTABLE':wyrebox_postfix_pipe.full_path (),}

    ,)
  test ('postfix lmtp delivery bridge', postfix_lmtp_delivery_bridge_test)
test ('postfix lmtp executable', postfix_lmtp_executable_test, depends: wyrebox_postfix_lmtp, env:{
'WYREBOX_POSTFIX_LMTP_EXECUTABLE':wyrebox_postfix_lmtp.full_path (),}

    ,)
test ('postfix lmtp delivery', postfix_lmtp_delivery_test, depends: wyrebox_postfix_lmtp_delivery, env:{
'WYREBOX_POSTFIX_LMTP_DELIVERY_EXECUTABLE':
    wyrebox_postfix_lmtp_delivery.full_path (),}

    ,)
test ('postfix pipe real daemon delivery', postfix_pipe_real_daemon_test, depends: [wyreboxd, wyrebox_postfix_pipe], env:{
'WYREBOXD_EXECUTABLE': wyreboxd.full_path (), 'WYREBOX_POSTFIX_PIPE_EXECUTABLE':wyrebox_postfix_pipe.full_path
      (),}
    ,)
    endif
