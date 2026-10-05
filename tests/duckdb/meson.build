duckdb_c_api_smoke_test = executable(
  'test-duckdb-c-api',
  'test-duckdb-c-api.c',
  dependencies: wyrebox_dep,
)

test(
  'duckdb c api smoke',
  duckdb_c_api_smoke_test,
)

duckdb_shared_test = executable(
  'test-duckdb-shared',
  'test-duckdb-shared.c',
  dependencies: wyrebox_dep,
)

test(
  'duckdb shared instance',
  duckdb_shared_test,
)
