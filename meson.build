project('wyrebox',
  'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>=1.0.0',
)

python = find_program('python3')
cc = meson.get_compiler('c')

capnp_option = get_option('capnp')
capnp = find_program('capnp', required: capnp_option)

capnp_schema_args = files('tests/docs/test_issue_0004_daemon_api_schema.py')
if capnp.found()
  capnp_schema_args += [
    '--capnp-mode=validate',
    '--capnp-path=' + capnp.full_path(),
  ]
  capnp_summary = capnp.full_path()
elif capnp_option.disabled()
  capnp_schema_args += ['--capnp-mode=skip']
  capnp_summary = 'disabled; schema syntax validation skipped'
else
  capnp_schema_args += ['--capnp-mode=skip']
  capnp_summary = 'not found; schema syntax validation skipped'
endif

summary('capnp', capnp_summary, section: 'Toolchain')

duckdb_option = get_option('duckdb')
duckdb_found = false
duckdb_deps = []
duckdb_api_probe = '''
  #include <stddef.h>
  #include <duckdb.h>

  int
  main (void)
  {
    duckdb_database db = NULL;

    if (duckdb_open (NULL, &db) == DuckDBSuccess)
      duckdb_close (&db);

    return 0;
  }
'''

if duckdb_option.disabled()
  duckdb_summary = 'disabled'
else
  duckdb_pkg_dep = dependency(
    'duckdb',
    method: 'pkg-config',
    required: false,
  )
  if duckdb_pkg_dep.found()
    duckdb_api_usable = cc.links(
      duckdb_api_probe,
      name: 'DuckDB C API via pkg-config',
      dependencies: duckdb_pkg_dep,
    )
    if duckdb_api_usable
      duckdb_found = true
      duckdb_deps = [duckdb_pkg_dep]
      duckdb_summary = 'enabled via pkg-config'
    elif duckdb_option.enabled()
      error('DuckDB dependency requested with -Dduckdb=enabled, but ' +
        'pkg-config found duckdb without a compile/link usable C API.')
    else
      duckdb_summary = (
        'not found; DuckDB integration disabled ' +
        '(pkg-config found duckdb, but the C API probe failed)'
      )
    endif
  else
    duckdb_lib_dep = cc.find_library('duckdb', required: false)
    duckdb_api_usable = false

    if duckdb_lib_dep.found()
      duckdb_api_usable = cc.links(
        duckdb_api_probe,
        name: 'DuckDB C API via library/header detection',
        dependencies: duckdb_lib_dep,
      )
    endif

    if duckdb_api_usable
      duckdb_found = true
      duckdb_deps = [duckdb_lib_dep]
      duckdb_summary = 'enabled via library/header detection'
    elif duckdb_option.enabled()
      error('DuckDB dependency requested with -Dduckdb=enabled, but ' +
        'pkg-config did not find duckdb and fallback detection did not ' +
        'find a compile/link usable duckdb.h and libduckdb C API.')
    else
      duckdb_summary = 'not found; DuckDB integration disabled'
    endif
  endif
endif

summary('duckdb', duckdb_summary, section: 'Dependencies')

glib_dep = dependency('glib-2.0', version: '>=2.74')
gobject_dep = dependency('gobject-2.0', version: '>=2.74')
gio_dep = dependency('gio-2.0', version: '>=2.74')

subdir('wyrebox')

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

test(
  'docs issue 0003 object store contract',
  python,
  args: files('tests/docs/test_issue_0003_object_store_contract.py'),
)

test(
  'docs issue 0003 mutation journal contract',
  python,
  args: files('tests/docs/test_issue_0003_mutation_journal_contract.py'),
)

test(
  'docs issue 0003 core schema contract',
  python,
  args: files('tests/docs/test_issue_0003_core_schema_contract.py'),
)

test(
  'docs issue 0003 gobject core components contract',
  python,
  args: files('tests/docs/test_issue_0003_gobject_core_components_contract.py'),
)

test(
  'docs issue 0004 error model contract',
  python,
  args: files('tests/docs/test_issue_0004_error_model_contract.py'),
)

test(
  'docs issue 0004 core daemon api contract',
  python,
  args: files('tests/docs/test_issue_0004_core_daemon_api_contract.py'),
)

test(
  'docs issue 0004 daemon api schema',
  python,
  args: capnp_schema_args,
)

subdir('tests/object-store')
subdir('tests/ingestion')
subdir('tests/journal')
subdir('tests/migration')
subdir('tests/build-config')
