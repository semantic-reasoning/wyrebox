test(
  'dovecot source contract',
  python,
  args: files('test-dovecot-source-contract.py'),
)

test(
  'dovecot plugin source contract',
  python,
  args: files('test-dovecot-plugin-source-contract.py'),
)

test(
  'dovecot build contract',
  python,
  args: files('test-dovecot-build-contract.py'),
)

test(
  'dovecot installed headers contract',
  python,
  args: files('test-dovecot-installed-headers-contract.py'),
)

test(
  'dovecot config include dir',
  python,
  args: files('test-dovecot-config-include-dir.py'),
)

# The setup tests run a nested meson setup, which needs the enclosing build's
# -Dduckdb_root on architectures without a bundled DuckDB prebuilt.
dovecot_setup_forwarded_args = []
if duckdb_root != ''
  dovecot_setup_forwarded_args += ['-Dduckdb_root=' + duckdb_root]
endif

test(
  'dovecot system setup',
  python,
  args: [files('test-dovecot-system-setup.py')] + dovecot_setup_forwarded_args,
  is_parallel: false,
  timeout: 900,
)

test(
  'dovecot fixture setup',
  python,
  args: [files('test-dovecot-fixture-setup.py')] + dovecot_setup_forwarded_args,
  is_parallel: false,
  timeout: 300,
)

dovecot_daemon_client_test = executable(
  'test-dovecot-daemon-client',
  'test-dovecot-daemon-client.c',
  dependencies: [wyrebox_dep],
)

test('dovecot daemon client', dovecot_daemon_client_test)

if have_dovecot_backend and dovecot_uses_system_headers
  test(
    'dovecot plugin symbols',
    python,
    args: [
      files('test-dovecot-plugin-symbols.py'),
      wyrebox_dovecot_plugin_module,
    ],
    depends: wyrebox_dovecot_plugin_module,
  )
endif

if capnp_serialization_found
  dovecot_imap_real_daemon_env = {
    'WYREBOXD_EXECUTABLE': wyreboxd.full_path(),
    'WYREBOX_POSTFIX_PIPE_EXECUTABLE': wyrebox_postfix_pipe.full_path(),
    'WYREBOX_EML_FIXTURE_DIR': meson.project_source_root() / 'tests' /
      'object-store' / 'fixtures',
  }
  dovecot_imap_real_daemon_depends = [wyreboxd, wyrebox_postfix_pipe]
  if have_dovecot_backend and dovecot_uses_system_headers
    dovecot_imap_real_daemon_env += {
      'WYREBOX_DOVECOT_PLUGIN': wyrebox_dovecot_plugin_module.full_path(),
    }
    dovecot_imap_real_daemon_depends += [wyrebox_dovecot_plugin_module]
  endif
  dovecot_imap = find_program(
    'imap',
    dirs: ['/usr/lib/dovecot'],
    required: false,
  )
  if dovecot_imap.found()
    dovecot_imap_real_daemon_env += {'DOVECOT_IMAP': dovecot_imap.full_path()}
  endif

  dovecot_imap_real_daemon_test = executable(
    'test-dovecot-imap-real-daemon',
    'test-dovecot-imap-real-daemon.c',
    dependencies: [wyrebox_dep],
  )
  dovecot_master = find_program(
    'dovecot',
    dirs: ['/usr/sbin'],
    required: false,
  )
  dovecot_imap_login = find_program(
    'imap-login',
    dirs: ['/usr/lib/dovecot'],
    required: false,
  )
  postfix_dovecot_imap_e2e_env = dovecot_imap_real_daemon_env + {
    'WYREBOX_POSTFIX_LMTP_DELIVERY_EXECUTABLE':
      wyrebox_postfix_lmtp_delivery.full_path(),
  }
  if dovecot_master.found()
    postfix_dovecot_imap_e2e_env += {
      'DOVECOT_MASTER': dovecot_master.full_path(),
    }
  endif
  if dovecot_imap_login.found()
    postfix_dovecot_imap_e2e_env += {
      'DOVECOT_IMAP_LOGIN': dovecot_imap_login.full_path(),
    }
  endif
  postfix_dovecot_imap_e2e_test = executable(
    'test-postfix-dovecot-imap-e2e',
    'test-postfix-dovecot-imap-e2e.c',
    dependencies: [wyrebox_dep],
  )
  test(
    'postfix dovecot imap e2e',
    postfix_dovecot_imap_e2e_test,
    depends: dovecot_imap_real_daemon_depends + [
      wyrebox_postfix_lmtp_delivery,
    ],
    env: postfix_dovecot_imap_e2e_env,
    timeout: 120,
  )

  test(
    'dovecot imap real daemon',
    dovecot_imap_real_daemon_test,
    depends: dovecot_imap_real_daemon_depends,
    env: dovecot_imap_real_daemon_env,
    timeout: 120,
  )
endif
