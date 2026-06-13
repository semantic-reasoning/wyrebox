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
