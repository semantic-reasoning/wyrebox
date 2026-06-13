project('wyrebox',
  'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>=1.0.0',
)

python = find_program('python3')
cc = meson.get_compiler('c')

if host_machine.system() != 'linux'
  error('WyreBox supports Linux only.')
endif

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

capnp_serialization_option = get_option('capnp_serialization')
capnp_serialization_found = false
capnp_serialization_compiler = find_program('capnp', required: false)
capnpc_cpp = find_program('capnpc-c++', required: false)
capnp_dep = dependency('capnp', method: 'pkg-config', required: false)
capnpc_dep = dependency('capnpc', method: 'pkg-config', required: false)
kj_dep = dependency('kj', method: 'pkg-config', required: false)
cpp_enabled = false

if capnp_serialization_option.disabled()
  capnp_serialization_summary = 'disabled'
elif capnp_serialization_compiler.found() and capnpc_cpp.found() and \
    capnp_dep.found() and capnpc_dep.found() and kj_dep.found()
  cpp_enabled = add_languages('cpp',
    required: capnp_serialization_option.enabled(),
    native: false,
  )
  if cpp_enabled
    capnp_serialization_found = true
    capnp_serialization_summary = 'enabled'
  elif capnp_serialization_option.enabled()
    error('Cap\'n Proto serialization requested with ' +
      '-Dcapnp_serialization=enabled, but no usable C++ compiler was found.')
  else
    capnp_serialization_summary = (
      'not found; generated API serialization disabled because no usable ' +
      'C++ compiler was found'
    )
  endif
elif capnp_serialization_option.enabled()
  error('Cap\'n Proto serialization requested with ' +
    '-Dcapnp_serialization=enabled, but capnp, capnpc-c++, and the ' +
    'capnp/capnpc/kj pkg-config runtime dependencies were not all found.')
else
  capnp_serialization_summary = (
    'not found; generated API serialization disabled'
  )
endif

summary('capnp serialization', capnp_serialization_summary,
  section: 'Toolchain')

if capnp_serialization_found
  daemon_api_capnp_schema = custom_target(
    'daemon-api-capnp-schema',
    input: files('wyrebox/wyrebox-daemon-api.capnp'),
    output: [
      'wyrebox-daemon-api.capnp.c++',
      'wyrebox-daemon-api.capnp.h',
    ],
    command: [
      capnp_serialization_compiler,
      'compile',
      '-oc++:@OUTDIR@',
      '--src-prefix=' + meson.project_source_root() / 'wyrebox',
      '@INPUT@',
    ],
  )

  daemon_api_capnp_schema_include = include_directories('.')
endif

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
  else
    error('pkg-config found duckdb, but it did not provide a ' +
      'compile/link usable C API.')
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
  else
    if host_machine.cpu_family() != 'x86_64'
      error('DuckDB is required, but the bundled DuckDB prebuilt ' +
        'subproject supports Linux x86_64 only.')
    endif

    duckdb_subproject_dep = dependency(
      'duckdb-prebuilt-linux',
      fallback: ['duckdb-prebuilt-linux', 'duckdb_dep'],
      required: true,
    )
    duckdb_api_usable = cc.links(
      duckdb_api_probe,
      name: 'DuckDB C API via linux prebuilt subproject',
      dependencies: duckdb_subproject_dep,
    )

    if not duckdb_api_usable
      error('DuckDB is required, but the DuckDB linux prebuilt ' +
        'subproject did not provide a compile/link usable C API.')
    endif

    duckdb_found = true
    duckdb_deps = [duckdb_subproject_dep]
    duckdb_summary = 'enabled via linux prebuilt subproject'
  endif
endif

summary('duckdb', duckdb_summary, section: 'Dependencies')

wirelog_runtime_option = get_option('wirelog_runtime')
wirelog_dep = dependency('wirelog', required: wirelog_runtime_option)
wirelog_runtime_found = wirelog_dep.found()
if wirelog_runtime_found
  wirelog_required_symbols = [
    'wirelog_parse_string',
    'wirelog_program_free',
    'wirelog_executor_create',
    'wirelog_executor_free',
    'wirelog_evaluate',
    'wirelog_result_relation_cardinality',
    'wirelog_result_write_csv',
    'wirelog_result_free',
    'wirelog_easy_open',
    'wirelog_easy_close',
    'wirelog_easy_intern',
    'wirelog_easy_snapshot',
  ]
  wirelog_missing_symbols = []
  foreach wirelog_symbol : wirelog_required_symbols
    if not cc.has_function(
      wirelog_symbol,
      prefix: '#include <wirelog/wirelog.h>',
      dependencies: wirelog_dep,
    )
      wirelog_missing_symbols += [wirelog_symbol]
    endif
  endforeach

  if wirelog_missing_symbols.length() > 0
    wirelog_missing_symbols_text = ', '.join(wirelog_missing_symbols)
    if wirelog_runtime_option.enabled()
      error('Wirelog runtime dependency is missing required public API ' +
        'symbols: ' + wirelog_missing_symbols_text)
    endif

    wirelog_dep = disabler()
    wirelog_runtime_found = false
    wirelog_runtime_summary = (
      'found but missing required public API symbols (' +
      wirelog_missing_symbols_text +
      '); optional runtime skipped'
    )
  else
    wirelog_runtime_summary = 'enabled via pkg-config'
  endif
elif wirelog_runtime_option.disabled()
  wirelog_runtime_summary = 'disabled'
else
  wirelog_runtime_summary = 'not found; optional smoke test skipped'
endif

summary('wirelog runtime', wirelog_runtime_summary, section: 'Dependencies')

glib_dep = dependency('glib-2.0', version: '>=2.74')
gobject_dep = dependency('gobject-2.0', version: '>=2.74')
gio_dep = dependency('gio-2.0', version: '>=2.74')
gio_unix_dep = dependency('gio-unix-2.0', version: '>=2.74')

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
  'docs issue 0004 deterministic fact extraction contract',
  python,
  args: files('tests/docs/test_issue_0004_deterministic_fact_extraction_contract.py'),
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
subdir('tests/daemon-api')
subdir('tests/facts')
subdir('tests/duckdb')
subdir('tests/wirelog')
