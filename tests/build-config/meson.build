duckdb_feature_expect = '0'
if duckdb_found
  duckdb_feature_expect = '1'
endif

capnp_serialization_feature_expect = '0'
if capnp_serialization_found
  capnp_serialization_feature_expect = '1'
endif

build_config_test = executable(
  'test-build-config',
  'test-build-config.c',
  c_args: [
    '-DWYREBOX_TEST_EXPECT_DUCKDB=' + duckdb_feature_expect,
    '-DWYREBOX_TEST_EXPECT_CAPNP_SERIALIZATION=' +
      capnp_serialization_feature_expect,
  ],
  dependencies: [wyrebox_dep],
)

test('build config feature macros', build_config_test)
