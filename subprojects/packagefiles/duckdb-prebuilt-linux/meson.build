project('duckdb-prebuilt-linux', 'c',
  version: '1.5.2',
  license: 'MIT',
)

cc = meson.get_compiler('c')
fs = import('fs')

if not fs.exists(meson.current_source_dir() / 'libduckdb.so')
  error('DuckDB prebuilt wrap did not provide libduckdb.so in the ' +
    'subproject source directory.')
endif

duckdb_lib = cc.find_library('duckdb',
  dirs: meson.current_source_dir(),
  required: true,
)

duckdb_dep = declare_dependency(
  dependencies: duckdb_lib,
  include_directories: include_directories('.'),
)
