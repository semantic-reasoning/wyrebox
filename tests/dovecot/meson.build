test(
  'dovecot source contract',
  python,
  args: files('test-dovecot-source-contract.py'),
)

test(
  'dovecot build contract',
  python,
  args: files('test-dovecot-build-contract.py'),
)

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
endif

if have_dovecot_loader_smoke
  dovecot_plugin_load_smoke = executable(
    'test-dovecot-plugin-load-smoke',
    'test-dovecot-plugin-load-smoke.c',
    c_args: dovecot_backend_c_args,
    dependencies: [dovecot_loader_dep],
  )

  test(
    'dovecot plugin loader smoke',
    dovecot_plugin_load_smoke,
    args: [
      meson.current_build_dir() / '..' / '..' / 'wyrebox',
    ],
    depends: wyrebox_dovecot_plugin_module,
  )
endif
