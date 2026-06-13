#include "wyrebox-build-config.h"

#ifndef WYREBOX_HAVE_DUCKDB
#error "WYREBOX_HAVE_DUCKDB must be defined"
#endif

#ifndef WYREBOX_DUCKDB_FROM_SUBPROJECT
#error "WYREBOX_DUCKDB_FROM_SUBPROJECT must be defined"
#endif

#ifndef WYREBOX_HAVE_CAPNP_SERIALIZATION
#error "WYREBOX_HAVE_CAPNP_SERIALIZATION must be defined"
#endif

#ifndef WYREBOX_TEST_EXPECT_CAPNP_SERIALIZATION
#error "WYREBOX_TEST_EXPECT_CAPNP_SERIALIZATION must be defined"
#endif

#if WYREBOX_HAVE_DUCKDB != 1
#error "WYREBOX_HAVE_DUCKDB must always be enabled"
#endif

#if WYREBOX_DUCKDB_FROM_SUBPROJECT != 1
#error "WYREBOX_DUCKDB_FROM_SUBPROJECT must always be enabled"
#endif

#if WYREBOX_HAVE_CAPNP_SERIALIZATION != 0 && \
    WYREBOX_HAVE_CAPNP_SERIALIZATION != 1
#error "WYREBOX_HAVE_CAPNP_SERIALIZATION must be numeric 0 or 1"
#endif

int
main (void)
{
  if (WYREBOX_HAVE_CAPNP_SERIALIZATION !=
      WYREBOX_TEST_EXPECT_CAPNP_SERIALIZATION)
    return 1;

  return 0;
}
