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

dovecot_daemon_client_test = executable(
  'test-dovecot-daemon-client',
  'test-dovecot-daemon-client.c',
  dependencies: [wyrebox_dep],
)

test('dovecot daemon client', dovecot_daemon_client_test)

if have_dovecot_backend
  test(
    'dovecot plugin symbols',
    python,
    args: [
      files('test-dovecot-plugin-symbols.py'),
      wyrebox_dovecot_plugin_module,
    ],
    depends: wyrebox_dovecot_plugin_module,
  )

  dovecot_plugin_mailbox_smoke = executable(
    'test-dovecot-plugin-mailbox-smoke',
    [
      'test-dovecot-plugin-mailbox-smoke.c',
      'test-dovecot-plugin-mailbox-smoke-stub-registration.c',
      meson.project_source_root() / 'wyrebox' / 'dovecot' / 'wyrebox-dovecot-plugin.c',
    ],
    c_args: dovecot_backend_c_args + ['-fvisibility=hidden'],
    dependencies: [wyrebox_dep],
  )

  test(
    'dovecot plugin mailbox smoke',
    dovecot_plugin_mailbox_smoke,
    env: {
      'WYREBOX_EML_FIXTURE_DIR': meson.project_source_root() /
        'tests' / 'object-store' / 'fixtures',
    },
  )
endif

if have_dovecot_loader_smoke
  dovecot_plugin_load_smoke_sources = ['test-dovecot-plugin-load-smoke.c']
  dovecot_plugin_load_smoke_c_args = dovecot_backend_c_args
  dovecot_plugin_load_smoke_link_args = ['-Wl,-export-dynamic']
  if not dovecot_loader_has_real_storage_registration
    dovecot_plugin_load_smoke_sources += ['test-dovecot-plugin-load-smoke-stub-registration.c']
    dovecot_plugin_load_smoke_c_args += ['-DWYREBOX_DOVECOT_LOADER_STUB_REGISTRATION=1']
  endif

  dovecot_plugin_load_smoke = executable(
    'test-dovecot-plugin-load-smoke',
    dovecot_plugin_load_smoke_sources,
    c_args: dovecot_plugin_load_smoke_c_args,
    link_args: dovecot_plugin_load_smoke_link_args,
    dependencies: [dovecot_loader_dep],
  )

  test(
    dovecot_loader_has_real_storage_registration ? \
      'dovecot plugin loader smoke' :
      'dovecot plugin loader smoke (shimmed registration)',
    dovecot_plugin_load_smoke,
    args: [
      meson.current_build_dir() / '..' / '..' / 'wyrebox',
    ],
    depends: wyrebox_dovecot_plugin_module,
  )

endif
