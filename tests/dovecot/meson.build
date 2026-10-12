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

test(
  'dovecot system setup',
  python,
  args: files('test-dovecot-system-setup.py'),
  is_parallel: false,
  timeout: 900,
)

test(
  'dovecot fixture setup',
  python,
  args: files('test-dovecot-fixture-setup.py'),
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
