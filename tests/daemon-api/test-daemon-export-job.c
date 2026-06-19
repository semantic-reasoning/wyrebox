#include "wyrebox-daemon-export-job-request.h"
#include "wyrebox-daemon-export-job-result.h"

#include <gio/gio.h>

static void
test_request_copies_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonExportJobRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_export_job_request_init (&request,
          "account-1", "messages.metadata.v1", "parquet", 4096, 7, 100,
          200, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (request.dataset_id, ==, "messages.metadata.v1");
  g_assert_cmpstr (request.output_format, ==, "parquet");
}

static void
test_request_rejects_reverse_time_range (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonExportJobRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_export_job_request_init (&request,
          "account-1", "messages.metadata.v1", "parquet", 0, 0, 200, 100,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_result_copies_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonExportJobResult) result = { 0 };

  g_assert_true (wyrebox_daemon_export_job_result_init (&result, "job-1",
          "account-1", "messages.metadata.v1",
          WYREBOX_DAEMON_EXPORT_JOB_STATUS_QUEUED, 10, 0, 0,
          "/exports/job-1.parquet", &error));
  g_assert_no_error (error);
  g_assert_cmpstr (result.job_id, ==, "job-1");
  g_assert_cmpstr (result.dataset_id, ==, "messages.metadata.v1");
  g_assert_cmpstr (result.result_object_path, ==, "/exports/job-1.parquet");
}

static void
test_result_rejects_invalid_timestamp_order (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonExportJobResult) result = { 0 };

  g_assert_false (wyrebox_daemon_export_job_result_init (&result, "job-1",
          "account-1", "messages.metadata.v1",
          WYREBOX_DAEMON_EXPORT_JOB_STATUS_RUNNING, 10, 5, 0, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/export-job/request-copies-fields",
      test_request_copies_fields);
  g_test_add_func ("/daemon-api/export-job/request-rejects-reverse-time-range",
      test_request_rejects_reverse_time_range);
  g_test_add_func ("/daemon-api/export-job/result-copies-fields",
      test_result_copies_fields);
  g_test_add_func
      ("/daemon-api/export-job/result-rejects-invalid-timestamp-order",
      test_result_rejects_invalid_timestamp_order);

  return g_test_run ();
}
