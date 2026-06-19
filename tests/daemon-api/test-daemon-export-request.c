#include "wyrebox-daemon-export-request.h"

#include <gio/gio.h>

static void
test_request_copies_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonExportRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_export_request_init (&request,
          "account-1", "messages.metadata.v1", "parquet", 4096, 7, 100,
          200, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.account_identity, ==, "account-1");
  g_assert_cmpstr (request.dataset_id, ==, "messages.metadata.v1");
  g_assert_cmpstr (request.output_format, ==, "parquet");
  g_assert_cmpuint (request.after_journal_offset, ==, 4096);
  g_assert_cmpuint (request.after_journal_sequence, ==, 7);
  g_assert_cmpuint (request.start_unix_us, ==, 100);
  g_assert_cmpuint (request.end_unix_us, ==, 200);
}

static void
test_request_rejects_reverse_time_range (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonExportRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_export_request_init (&request,
          "account-1", "messages.metadata.v1", "parquet", 0, 0, 200, 100,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_request_rejects_missing_dataset_id (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonExportRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_export_request_init (&request,
          "account-1", "", "parquet", 0, 0, 0, 0, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.dataset_id);
}

static void
test_request_rejects_half_open_cursor_offset_only (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonExportRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_export_request_init (&request,
          "account-1", "messages.metadata.v1", "parquet", 4096, 0, 0, 0,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/export-request/copies-fields",
      test_request_copies_fields);
  g_test_add_func ("/daemon-api/export-request/rejects-reverse-time-range",
      test_request_rejects_reverse_time_range);
  g_test_add_func ("/daemon-api/export-request/rejects-missing-dataset-id",
      test_request_rejects_missing_dataset_id);
  g_test_add_func ("/daemon-api/export-request/rejects-half-open-cursor",
      test_request_rejects_half_open_cursor_offset_only);

  return g_test_run ();
}
