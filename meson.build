project('wyrebox',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>=1.0.0',
)

python = find_program('python3')

test(
  'docs issue 0001 postfix ingress adr',
  python,
  args: files('tests/docs/test_issue_0001_postfix_ingress_adr.py'),
)

test(
  'docs issue 0001 dovecot boundary adr',
  python,
  args: files('tests/docs/test_issue_0001_dovecot_boundary_adr.py'),
)

test(
  'docs issue 0001 mailbox storage model contract',
  python,
  args: files('tests/docs/test_issue_0001_mailbox_storage_model_contract.py'),
)

test(
  'docs issue 0001 postfix dovecot verification spike',
  python,
  args: files('tests/docs/test_issue_0001_postfix_dovecot_verification_spike.py'),
)

test(
  'docs issue 0002 linux runtime contract',
  python,
  args: files('tests/docs/test_issue_0002_linux_runtime_contract.py'),
)
