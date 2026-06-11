#include "wyrebox-build-config.h"

#ifndef WYREBOX_HAVE_DUCKDB
#error "WYREBOX_HAVE_DUCKDB must be defined"
#endif

#ifndef WYREBOX_TEST_EXPECT_DUCKDB
#error "WYREBOX_TEST_EXPECT_DUCKDB must be defined"
#endif

#if WYREBOX_HAVE_DUCKDB != 0 && WYREBOX_HAVE_DUCKDB != 1
#error "WYREBOX_HAVE_DUCKDB must be numeric 0 or 1"
#endif

int
main (void)
{
  return WYREBOX_HAVE_DUCKDB == WYREBOX_TEST_EXPECT_DUCKDB ? 0 : 1;
}
