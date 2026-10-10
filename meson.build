project('wyrebox',
  'c',
  version: '0.1.0',
  license: 'MPL-2.0',
  meson_version: '>=1.1.0',
)

fs = import('fs')
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

# DuckDB provisioning is architecture-aware. On x86_64 the build uses the
# bundled prebuilt subproject wrap. On other architectures (e.g. arm64) no
# prebuilt is bundled, so DuckDB must be built from source out-of-band and
# pointed at via -Dduckdb_root=<dir> (containing include/duckdb.h and
# lib/libduckdb.so). In every case WyreBox provides DuckDB itself rather than
# resolving a system provider.
duckdb_root = get_option('duckdb_root')

if duckdb_root != ''
  duckdb_lib = cc.find_library('duckdb',
    dirs: duckdb_root / 'lib',
    required: true,
  )
  duckdb_subproject_dep = declare_dependency(
    dependencies: duckdb_lib,
    compile_args: ['-I' + duckdb_root / 'include'],
  )
  duckdb_api_name = 'DuckDB C API via source-built duckdb_root'
  duckdb_summary = 'enabled via source-built DuckDB at ' + duckdb_root
else
  if host_machine.cpu_family() == 'x86_64'
    # Use the wrapped DuckDB subproject directly: dependency fallback can
    # resolve a system provider, which this build intentionally avoids.
    duckdb_subproject = subproject('duckdb-prebuilt-linux')
    duckdb_subproject_dep = duckdb_subproject.get_variable('duckdb_dep')
    duckdb_api_name = 'DuckDB C API via linux prebuilt subproject'
    duckdb_summary = 'enabled via linux prebuilt subproject wrap'
  else
    error('DuckDB is required, but the bundled DuckDB prebuilt subproject ' +
      'supports Linux x86_64 only; on other architectures build DuckDB from ' +
      'source and pass -Dduckdb_root=<dir> (containing include/ and lib/).')
  endif
endif

duckdb_api_usable = cc.links(
  duckdb_api_probe,
  name: duckdb_api_name,
  dependencies: duckdb_subproject_dep,
)

if not duckdb_api_usable
  error('DuckDB is required, but the configured DuckDB provider did not ' +
    'provide a compile/link usable C API.')
endif

duckdb_deps = [duckdb_subproject_dep]
# Always true: WyreBox bundles/provides DuckDB (prebuilt wrap or source build),
# never a system package. See tests/build-config/test-build-config.c.
duckdb_from_subproject = true
summary('duckdb', duckdb_summary, section: 'Dependencies')

wirelog_runtime_option = get_option('wirelog_runtime')
if wirelog_runtime_option.disabled()
  error('Wirelog runtime is required; ' +
    '-Dwirelog_runtime=disabled is not supported')
endif

wirelog_force_fallback = get_option('wrap_mode') == 'forcefallback'
if wirelog_force_fallback
  wirelog_dep = disabler()
else
  wirelog_dep = dependency('wirelog',
    version: '==0.70.0',
    required: false,
    allow_fallback: false)
endif
wirelog_runtime_summary = 'required via pkg-config'
wirelog_from_subproject = false
if not wirelog_dep.found()
  wirelog_project = subproject(
    'wirelog',
    default_options: [
      'tests=false',
      'documentation=false',
      'ipc=disabled',
      'mbedTLS=disabled',
      'io_plugin_dlopen=disabled',
      'wirelog_log_max_level=error',
    ],
  )
  wirelog_dep = declare_dependency(
    link_with: wirelog_project.get_variable('wirelog_lib'),
    include_directories: [
      wirelog_project.get_variable('wirelog_inc'),
      wirelog_project.get_variable('wirelog_src_inc'),
    ],
  )
  wirelog_runtime_summary = 'required via subproject fallback'
  wirelog_from_subproject = true
endif
wirelog_runtime_found = true
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
if not wirelog_from_subproject
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
    error('Wirelog runtime dependency is missing required public API symbols: ' +
      wirelog_missing_symbols_text)
  endif
endif

summary('wirelog runtime', wirelog_runtime_summary, section: 'Dependencies')

# libchronoid provides UUIDv7 storage identifiers. It is LGPL-3.0-or-later,
# so WyreBox links its shared library only.
if get_option('wrap_mode') == 'forcefallback' or \
    'libchronoid' in get_option('force_fallback_for')
  chronoid_dep = dependency('', required: false)
else
  chronoid_dep = dependency('libchronoid',
    version: ['>=1.2.0', '<2'],
    required: false,
    allow_fallback: false)
endif
chronoid_summary = 'shared library via pkg-config'
if not chronoid_dep.found()
  chronoid_project = subproject(
    'libchronoid',
    default_options: [
      'tests=false',
      'cli=false',
    ],
  )
  chronoid_dep = declare_dependency(
    link_with: chronoid_project.get_variable('chronoid_lib').get_shared_lib(),
    include_directories: chronoid_project.get_variable('inc'),
  )
  chronoid_summary = 'shared library via subproject fallback'
endif
summary('libchronoid', chronoid_summary, section: 'Dependencies')

dovecot_backend_option = get_option('dovecot_backend')
dovecot_loader_smoke_option = get_option('dovecot_loader_smoke')
dovecot_source_dir_option = get_option('dovecot_source_dir')
dovecot_build_dir_option = get_option('dovecot_build_dir')
dovecot_config_option = get_option('dovecot_config')
dovecot_loader_archive_option = get_option('dovecot_loader_archive')
dovecot_loader_extra_link_args = get_option('dovecot_loader_extra_link_args')
have_dovecot_backend = false
dovecot_uses_system_headers = false
have_dovecot_loader_smoke = false
dovecot_loader_has_real_storage_registration = false
dovecot_backend_c_args = []
dovecot_loader_dep = disabler()
if dovecot_backend_option.enabled()
  if dovecot_config_option != ''
    if dovecot_source_dir_option != '' or dovecot_build_dir_option != ''
      error(
        '-Ddovecot_config cannot be combined with -Ddovecot_source_dir or ' +
        '-Ddovecot_build_dir',
      )
    endif
    if dovecot_loader_smoke_option.enabled()
      error(
        'Dovecot loader smoke is not supported with -Ddovecot_config; it ' +
        'needs liblib.a from a Dovecot source build',
      )
    endif
    dovecot_config = meson.current_source_dir() / dovecot_config_option
    dovecot_include_lookup = run_command(
      python,
      meson.current_source_dir() / 'tools' / 'dovecot-config-include-dir.py',
      dovecot_config,
      check: false,
    )
    if dovecot_include_lookup.returncode() != 0
      error(
        'Dovecot backend is enabled but dovecot-config could not be used:\n' +
        dovecot_include_lookup.stderr().strip(),
      )
    endif
    dovecot_include_dir = dovecot_include_lookup.stdout().strip()
    dovecot_uses_system_headers = true
    dovecot_source_contract_args = ['--include-dir', dovecot_include_dir]
    dovecot_build_contract_args = ['--include-dir', dovecot_include_dir]
    dovecot_backend_c_args = [
      '-std=gnu11',
      '-I' + dovecot_include_dir,
    ]
  else
    if dovecot_build_dir_option == ''
      error(
        'Dovecot backend is enabled but no configured build directory was ' +
        'provided; set -Ddovecot_build_dir=/path/to/dovecot/build or ' +
        '-Ddovecot_config=/usr/lib/dovecot/dovecot-config',
      )
    endif
    if dovecot_source_dir_option == ''
      error('Dovecot backend is enabled but no source directory was provided; set ' +
        '-Ddovecot_source_dir=/path/to/dovecot')
    endif
    dovecot_source_dir = meson.current_source_dir() / dovecot_source_dir_option
    dovecot_build_dir = meson.current_source_dir() / dovecot_build_dir_option
    dovecot_source_contract_args = [dovecot_source_dir]
    dovecot_build_contract_args = [dovecot_source_dir, dovecot_build_dir]
    dovecot_backend_c_args = [
      '-std=gnu11',
      '-I' + (dovecot_source_dir / 'src' / 'lib-index'),
      '-I' + (dovecot_source_dir / 'src' / 'lib'),
      '-I' + (dovecot_source_dir / 'src' / 'lib-mail'),
      '-I' + (dovecot_source_dir / 'src' / 'lib-storage'),
      '-I' + dovecot_build_dir,
    ]
  endif

  check_dovecot_source = run_command(
    python,
    meson.current_source_dir() / 'tools' / 'check-dovecot-source-contract.py',
    dovecot_source_contract_args,
    check: false,
  )
  if check_dovecot_source.returncode() != 0
    check_error = check_dovecot_source.stderr().strip()
    if check_error == ''
      check_error = check_dovecot_source.stdout().strip()
    endif
    error(
      'Dovecot backend is enabled but source contract validation failed:\n' +
      check_error,
    )
  endif

  check_dovecot_build = run_command(
    python,
    meson.current_source_dir() / 'tools' / 'check-dovecot-build-contract.py',
    dovecot_build_contract_args,
    check: false,
  )
  if check_dovecot_build.returncode() != 0
    check_error = check_dovecot_build.stderr().strip()
    if check_error == ''
      check_error = check_dovecot_build.stdout().strip()
    endif
    error(
      'Dovecot backend is enabled but configured build contract validation failed:\n' +
      check_error,
    )
  endif

  dovecot_configured_header_probe = '''
    #include "config.h"
    #include "lib.h"
    #include "module-dir.h"
    #include "mail-storage.h"
    #include "mail-storage-hooks.h"

    #ifndef DOVECOT_ABI_VERSION
    #error DOVECOT_ABI_VERSION must be defined by the configured build config.h
    #endif

    #ifndef OFF_T_MAX
    #error OFF_T_MAX must be defined by the configured build config.h
    #endif

    #ifndef PRIuUOFF_T
    #error PRIuUOFF_T must be defined by the configured build config.h
    #endif

    #ifndef SIZEOF_INT
    #error SIZEOF_INT must be defined by the configured build config.h
    #endif

    #ifndef SIZEOF_LONG
    #error SIZEOF_LONG must be defined by the configured build config.h
    #endif

    #ifndef SIZEOF_VOID_P
    #error SIZEOF_VOID_P must be defined by the configured build config.h
    #endif

    #ifndef SSIZE_T_MAX
    #error SSIZE_T_MAX must be defined by the configured build config.h
    #endif

    #ifndef UOFF_T_MAX
    #error UOFF_T_MAX must be defined by the configured build config.h
    #endif

    #if !defined(HAVE_UOFF_T) && !defined(UOFF_T_INT) && \
        !defined(UOFF_T_LONG) && !defined(UOFF_T_LONG_LONG)
    #error config.h must define a uoff_t selector macro
    #endif

    int
    main (void)
    {
      uoff_t offset = 0;
      struct module *module = 0;
      struct mail_storage_hooks hooks = { 0 };
      struct mailbox_status status = { 0 };

      (void) offset;
      (void) module;
      (void) hooks;
      (void) status;
      (void) PRIuUOFF_T;

      return (UOFF_T_MAX == 0 || SSIZE_T_MAX == 0 || OFF_T_MAX == 0) ? 1 : 0;
    }
  '''

  check_dovecot_build_headers = cc.compiles(
    dovecot_configured_header_probe,
    name: 'Dovecot configured header contract check',
    args: dovecot_backend_c_args,
  )

  if not check_dovecot_build_headers
    error(
      'Dovecot backend is enabled but configured header contract validation failed '
      +
      'for configured build and source directories.',
    )
  endif

  dovecot_abi_version = cc.get_define(
    'DOVECOT_ABI_VERSION',
    prefix: '#include "config.h"',
    args: dovecot_backend_c_args,
  )
  if dovecot_abi_version != '"2.4.ABIv2"'
    error(
      'Dovecot backend requires DOVECOT_ABI_VERSION "2.4.ABIv2", got ' +
      dovecot_abi_version,
    )
  endif

  have_dovecot_backend = true
endif

if dovecot_loader_smoke_option.enabled()
  if not have_dovecot_backend
    error(
      'Dovecot loader smoke is enabled but the Dovecot backend is not; set ' +
      '-Ddovecot_backend=enabled with Dovecot source/build directories.',
    )
  endif
  if dovecot_loader_archive_option == ''
    error(
      'Dovecot loader smoke is enabled but no loader archive was provided; ' +
      'set -Ddovecot_loader_archive=/path/to/dovecot/src/lib/.libs/liblib.a',
    )
  endif

  dovecot_loader_archive = (
    meson.current_source_dir() / dovecot_loader_archive_option
  )
  if not fs.is_file(dovecot_loader_archive)
    error(
      'Dovecot loader smoke archive does not exist or is not a file: ' +
      dovecot_loader_archive,
    )
  endif

  check_dovecot_loader_storage_symbols = run_command(
    python,
    meson.current_source_dir() / 'tools' / 'check-dovecot-loader-storage-symbols.py',
    dovecot_loader_archive,
    check: false,
  )
  dovecot_loader_has_real_storage_registration = (
    check_dovecot_loader_storage_symbols.returncode() == 0
  )

  if not dovecot_loader_has_real_storage_registration
    warning(
      'Dovecot loader archive does not define storage registration symbols: ' +
      check_dovecot_loader_storage_symbols.stderr().strip(),
    )
  endif

  if not dovecot_loader_has_real_storage_registration
    message(
      'Dovecot loader archive does not expose storage registration symbols; ' +
      'loader smoke test will run with explicit registration shim symbols.',
    )
  endif

  dovecot_loader_link_args = []
  if dovecot_loader_has_real_storage_registration
    dovecot_loader_link_args += [
      '-Wl,-u,mail_storage_class_register',
      '-Wl,-u,mail_storage_class_unregister',
    ]
  endif
  dovecot_loader_link_args += [
    dovecot_loader_archive,
  ] + dovecot_loader_extra_link_args
  dovecot_loader_dep = declare_dependency(
    link_args: dovecot_loader_link_args,
  )

  dovecot_loader_probe = '''
    #include "config.h"
    #include "lib.h"
    #include "module-dir.h"

    int
    main (void)
    {
      struct module *modules = 0;
      struct module_dir_load_settings settings = { 0 };

      settings.abi_version = DOVECOT_ABI_VERSION;
      settings.require_init_funcs = 1;

      lib_init ();
      modules = module_dir_load (".", NULL, &settings);
      module_dir_unload (&modules);
      lib_deinit ();

      return settings.require_init_funcs ? 0 : 1;
    }
  '''
  if not cc.links(
    dovecot_loader_probe,
    name: 'Dovecot module loader smoke link check',
    args: dovecot_backend_c_args,
    dependencies: [dovecot_loader_dep],
  )
    if dovecot_loader_has_real_storage_registration
      error(
        'Dovecot loader smoke is enabled but the supplied loader archive ' +
        'could not be linked. Set -Ddovecot_loader_archive to the Dovecot ' +
        'src/lib/.libs/liblib.a artifact and add any required system libraries ' +
        'with -Ddovecot_loader_extra_link_args=...',
      )
    endif

    message(
      'Dovecot loader smoke is enabled but the supplied loader archive could ' +
      'not be linked; loader smoke is disabled because registration validation ' +
      'cannot be executed.',
    )
  else
    have_dovecot_loader_smoke = true
  endif
endif

summary(
  'dovecot backend',
  have_dovecot_backend ? 'enabled' : 'disabled',
  section: 'Features',
)
summary(
  'dovecot headers',
  dovecot_uses_system_headers ? 'system (dovecot-config)' : 'source/build tree',
  section: 'Features',
)
summary(
  'dovecot loader smoke',
  have_dovecot_loader_smoke ? 'enabled' : 'disabled',
  section: 'Features',
)
summary(
  'dovecot loader smoke registration',
  dovecot_loader_has_real_storage_registration ? 'real symbols' : 'shimmed validation only',
  section: 'Features',
)

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
  'docs duckdb dependency contract',
  python,
  args: files('tests/docs/test_duckdb_dependency_contract.py'),
)

test(
  'docs issue 0337 libchronoid dependency',
  python,
  args: files('tests/docs/test_issue_0337_libchronoid_dependency.py'),
)

test(
  'docs issue 0337 storage initialization',
  python,
  args: files('tests/docs/test_issue_0337_storage_initialization.py'),
)

test(
  'docs issue 0004 daemon api schema',
  python,
  args: capnp_schema_args,
)

test(
  'docs issue 0007 postfix pipe examples',
  python,
  args: files('tests/docs/test_issue_0007_postfix_pipe_examples.py'),
)

test(
  'docs issue 0028 benchmark scaffold',
  python,
  args: files('tests/benchmarks/test_issue_0028_benchmark_scaffold.py'),
)

test(
  'docs issue 0028 journal writer benchmark scaffold',
  python,
  args: files('tests/benchmarks/test_issue_0028_journal_writer_benchmark_scaffold.py'),
)

test(
  'docs issue 0028 object store benchmark scaffold',
  python,
  args: files('tests/benchmarks/test_issue_0028_object_store_benchmark_scaffold.py'),
)

test(
  'docs issue 0028 object store get benchmark scaffold',
  python,
  args: files('tests/benchmarks/test_issue_0028_object_store_get_benchmark_scaffold.py'),
)

test(
  'docs issue 0028 duckdb query template subject contains benchmark scaffold',
  python,
  args: files('tests/benchmarks/test_issue_0028_duckdb_query_template_subject_contains_benchmark_scaffold.py'),
)

test(
  'docs issue 0029 postfix pipe install',
  python,
  args: files('tests/docs/test_issue_0029_postfix_pipe_install.py'),
)

if capnp_serialization_found
  postfix_pipe_install_smoke_args = [
    files('tests/docs/test_issue_0029_postfix_pipe_install_smoke.py'),
  ]
  if duckdb_root != ''
    postfix_pipe_install_smoke_args += ['-Dduckdb_root=' + duckdb_root]
  endif
  test(
    'docs issue 0029 postfix pipe install smoke',
    python,
    args: postfix_pipe_install_smoke_args,
    timeout: 600,
  )
endif

test(
  'docs issue 0011 postfix lmtp examples',
  python,
  args: files('tests/docs/test_issue_0011_postfix_lmtp_examples.py'),
)

test(
  'docs issue 0025 backup restore workflow contract',
  python,
  args: files('tests/docs/test_issue_0025_backup_restore_workflow_contract.py'),
)

if fs.is_file('/usr/lib/dovecot/libdovecot-storage.so')
  test(
    'docs issue 0289 postfix dovecot e2e recipe',
    python,
    args: files('tests/docs/test_issue_0289_postfix_dovecot_e2e_recipe.py'),
    is_parallel: false,
  )

  test(
    'docs issue 0293 dovecot imap fetch recipe',
    python,
    args: files('tests/docs/test_issue_0293_dovecot_imap_fetch_recipe.py'),
    is_parallel: false,
  )

  test(
    'docs issue 0292 postfix to dovecot e2e recipe',
    python,
    args: files('tests/docs/test_issue_0292_postfix_to_dovecot_e2e_recipe.py'),
    is_parallel: false,
  )
endif

test(
  'docs issue 0298 dovecot fixture bundle contract',
  python,
  args: files('tests/docs/test_issue_0298_dovecot_fixture_bundle_contract.py'),
)

test(
  'docs issue 0317 materialization isolation adr',
  python,
  args: files('tests/docs/test_issue_0317_materialization_isolation_adr.py'),
)

test(
  'docs issue 0354 dovecot 2.4 baseline adr',
  python,
  args: files('tests/docs/test_issue_0354_dovecot_24_baseline_adr.py'),
)

test(
  'docs issue 0331 object error classification',
  python,
  args: files('tests/docs/test_issue_0331_object_error_classification.py'),
)

test(
  'docs issue 0326 ci capnp configurations',
  python,
  args: files('tests/docs/test_issue_0326_ci_capnp_configurations.py'),
)

test(
  'docs issue 0354 ubuntu 26.04 ci',
  python,
  args: files('tests/docs/test_issue_0354_ubuntu_26_04_ci.py'),
)

# Activate the committed hook through a worktree-relative hooksPath. Source
# archives and unpacked sources nested below another repository deliberately
# skip this developer setup.
git_program = find_program('git', required: false)
if git_program.found()
  posix_sh = find_program('sh')
  git_toplevel = run_command(
    git_program,
    '-C', meson.project_source_root(),
    'rev-parse', '--show-toplevel',
    check: false,
  )
  physical_source_root = run_command(
    posix_sh,
    '-c', 'CDPATH= cd "$1" && pwd -P',
    'sh', meson.project_source_root(),
    check: false,
  )
  if git_toplevel.returncode() == 0 and \
      physical_source_root.returncode() == 0
    physical_git_toplevel = run_command(
      posix_sh,
      '-c', 'CDPATH= cd "$1" && pwd -P',
      'sh', git_toplevel.stdout().strip(),
      check: false,
    )
    if physical_git_toplevel.returncode() == 0 and \
        physical_git_toplevel.stdout().strip() == \
        physical_source_root.stdout().strip()
      git_hook_setup = find_program('tools/setup-git-hooks.sh')
      message('Activating committed git hooks')
      run_command(git_hook_setup, check: true)
    endif
  endif
endif

subdir('tests/object-store')
subdir('tests/backup')
subdir('tests/admin')
subdir('tests/ingestion')
subdir('tests/maildir')
subdir('tests/journal')
subdir('tests/migration')
subdir('tests/build-config')
subdir('tests/daemon-api')
subdir('tests/dovecot')
subdir('tests/postfix')
subdir('tests/facts')
subdir('tests/duckdb')
subdir('tests/wirelog')
subdir('tests/recovery')
subdir('tests/benchmarks')
