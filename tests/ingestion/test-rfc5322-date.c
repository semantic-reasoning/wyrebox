#include "wyrebox-rfc5322-date.h"

#include <glib.h>

static void
assert_date_parses_to (const gchar *value, gint64 expected_unix_us)
{
  gint64 unix_us = 0;

  g_assert_true (wyrebox_rfc5322_date_parse_unix_us (value, &unix_us));
  g_assert_cmpint (unix_us, ==, expected_unix_us);
}

static void
assert_date_is_rejected (const gchar *value)
{
  gint64 unix_us = 123;

  g_assert_false (wyrebox_rfc5322_date_parse_unix_us (value, &unix_us));
  g_assert_cmpint (unix_us, ==, 0);
}

static void
test_weekday_must_match_local_date (void)
{
  assert_date_parses_to ("Friday, 12 Jun 2026 00:30:00 +1400",
      G_GINT64_CONSTANT (1781173800000000));
  assert_date_is_rejected ("Thu, 12 Jun 2026 05:00:00 -0500");
}

static void
test_bogus_weekday_token_is_rejected (void)
{
  assert_date_is_rejected ("Bogus, 12 Jun 2026 05:00:00 -0500");
}

static void
test_year_policy (void)
{
  assert_date_parses_to ("Fri, 12 Jun 26 05:00:00 -0500",
      G_GINT64_CONSTANT (1781258400000000));
  assert_date_parses_to ("Fri, 12 Jun 126 05:00:00 -0500",
      G_GINT64_CONSTANT (1781258400000000));
  assert_date_is_rejected ("Fri, 12 Jun 6 05:00:00 -0500");
  assert_date_is_rejected ("Fri, 12 Jun 1899 05:00:00 -0500");
}

static void
test_two_digit_year_pivot_policy (void)
{
  assert_date_parses_to ("Sat, 12 Jun 49 10:00:00 +0000",
      G_GINT64_CONSTANT (2507104800000000));
  assert_date_parses_to ("Mon, 12 Jun 50 10:00:00 +0000",
      G_GINT64_CONSTANT (-617119200000000));
  assert_date_parses_to ("Sat, 01 Jan 00 00:00:00 +0000",
      G_GINT64_CONSTANT (946684800000000));
  assert_date_parses_to ("Fri, 31 Dec 99 23:59:59 +0000",
      G_GINT64_CONSTANT (946684799000000));
}

static void
test_four_digit_year_floor (void)
{
  assert_date_parses_to ("Mon, 01 Jan 1900 00:00:00 +0000",
      G_GINT64_CONSTANT (-2208988800000000));
  assert_date_is_rejected ("Sun, 31 Dec 1899 23:59:59 +0000");
}

static void
test_timezone_numeric_bounds (void)
{
  assert_date_is_rejected ("Fri, 12 Jun 2026 05:00:00 +2400");
  assert_date_is_rejected ("Fri, 12 Jun 2026 05:00:00 -2400");
  assert_date_is_rejected ("Fri, 12 Jun 2026 05:00:00 +2360");
  assert_date_is_rejected ("Fri, 12 Jun 2026 05:00:00 -2360");
  assert_date_is_rejected ("Fri, 12 Jun 2026 05:00:00 +2A00");
}

static void
test_timezone_offsets_cross_utc_day_boundaries (void)
{
  assert_date_parses_to ("Friday, 12 Jun 2026 00:30:00 +1400",
      G_GINT64_CONSTANT (1781173800000000));
  assert_date_parses_to ("Fri, 12 Jun 2026 01:30:00 +0300",
      G_GINT64_CONSTANT (1781217000000000));
  assert_date_parses_to ("Fri, 12 Jun 2026 23:30:00 -0200",
      G_GINT64_CONSTANT (1781314200000000));
  assert_date_parses_to ("Friday, 12 Jun 2026 23:15:00 -1400",
      G_GINT64_CONSTANT (1781356500000000));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/ingestion/rfc5322-date/weekday-local-date",
      test_weekday_must_match_local_date);
  g_test_add_func ("/ingestion/rfc5322-date/bogus-weekday",
      test_bogus_weekday_token_is_rejected);
  g_test_add_func ("/ingestion/rfc5322-date/year-policy", test_year_policy);
  g_test_add_func ("/ingestion/rfc5322-date/two-digit-year-pivot",
      test_two_digit_year_pivot_policy);
  g_test_add_func ("/ingestion/rfc5322-date/four-digit-year-floor",
      test_four_digit_year_floor);
  g_test_add_func ("/ingestion/rfc5322-date/timezone-numeric-bounds",
      test_timezone_numeric_bounds);
  g_test_add_func ("/ingestion/rfc5322-date/timezone-utc-day-boundaries",
      test_timezone_offsets_cross_utc_day_boundaries);

  return g_test_run ();
}
