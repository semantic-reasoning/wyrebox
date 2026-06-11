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
