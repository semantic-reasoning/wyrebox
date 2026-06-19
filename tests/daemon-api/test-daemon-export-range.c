#include "wyrebox-daemon-export-range.h"

#include <glib.h>

static void
test_valid_journal_offset_range (void)
{
  g_auto (WyreboxDaemonExportRange) range = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_export_range_init (&range,
          "account-1", "events.stream.v1", "cursor-1",
          WYREBOX_DAEMON_EXPORT_RANGE_KIND_JOURNAL_OFFSET,
          17, 19, 0, 0, &error));
  g_assert_no_error (error);
}

static void
test_valid_time_range (void)
{
  g_auto (WyreboxDaemonExportRange) range = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_export_range_init (&range,
          "account-1", "messages.metadata.v1", NULL,
          WYREBOX_DAEMON_EXPORT_RANGE_KIND_TIME_RANGE, 0, 0, 100, 200, &error));
  g_assert_no_error (error);
}

static void
test_time_range_rejects_journal_cursor (void)
{
  g_auto (WyreboxDaemonExportRange) range = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_export_range_init (&range,
          "account-1", "messages.metadata.v1", NULL,
          WYREBOX_DAEMON_EXPORT_RANGE_KIND_TIME_RANGE, 7, 1, 100, 200, &error));
  g_assert_nonnull (error);
}

static void
test_journal_range_rejects_time_window (void)
{
  g_auto (WyreboxDaemonExportRange) range = { 0 };
  g_autoptr (GError) error = NULL;

  g_assert_false (wyrebox_daemon_export_range_init (&range,
          "account-1", "events.stream.v1", "cursor-1",
          WYREBOX_DAEMON_EXPORT_RANGE_KIND_JOURNAL_OFFSET,
          7, 1, 100, 200, &error));
  g_assert_nonnull (error);
}

static void
test_export_range_kind_names_are_stable (void)
{
  g_assert_cmpstr (wyrebox_daemon_export_range_kind_to_string
      (WYREBOX_DAEMON_EXPORT_RANGE_KIND_TIME_RANGE), ==, "time-range");
  g_assert_null (wyrebox_daemon_export_range_kind_to_string (
          (WyreboxDaemonExportRangeKind) 0));
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/export-range/valid-journal-offset",
      test_valid_journal_offset_range);
  g_test_add_func ("/daemon-api/export-range/valid-time-range",
      test_valid_time_range);
  g_test_add_func ("/daemon-api/export-range/time-range-rejects-cursor",
      test_time_range_rejects_journal_cursor);
  g_test_add_func ("/daemon-api/export-range/journal-range-rejects-time-window",
      test_journal_range_rejects_time_window);
  g_test_add_func ("/daemon-api/export-range/kind-names",
      test_export_range_kind_names_are_stable);

  return g_test_run ();
}
