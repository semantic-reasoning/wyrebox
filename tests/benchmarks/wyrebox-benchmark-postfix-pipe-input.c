#include "wyrebox-benchmark-report.h"
#include "wyrebox-postfix-pipe-input.h"

#include <gio/gio.h>
#include <glib.h>

static GInputStream *
stream_from_data (const guint8 *data, gsize size)
{
  return g_memory_input_stream_new_from_data (data, (gssize) size, NULL);
}

static void
run_microbenchmark (void)
{
  static const guint8 message[] =
      "From: sender@example.test\r\n"
      "To: recipient@example.test\r\n"
      "Subject: wyrebox postfix pipe benchmark\r\n"
      "Date: Thu, 01 Jan 1970 00:00:00 +0000\r\n"
      "Message-ID: <wyrebox-postfix-pipe-benchmark@example.test>\r\n"
      "\r\n" "fixed RFC 5322 payload\r\n";
  static const char *argv[] = {
    "wyrebox-postfix-pipe",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--queue-id", "queue-1",
    "--sender", "sender@example.com",
    "--recipient", "recipient@example.com",
    NULL
  };
  g_auto (WyreboxBenchmarkReport) report = { 0 };
  g_auto (WyreboxPostfixPipeOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GInputStream) input = NULL;
  g_autoptr (GError) error = NULL;
  gint64 start_us = 0;
  gint64 end_us = 0;
  int argc = 0;

  while (argv[argc] != NULL)
    argc++;

  input = stream_from_data (message, sizeof (message) - 1);
  g_assert_nonnull (input);

  wyrebox_benchmark_report_init (&report);

  start_us = g_get_monotonic_time ();
  g_assert_true (wyrebox_postfix_pipe_input_build (argc,
          argv, input, 4096, &options, &identity, &request, &error));
  end_us = g_get_monotonic_time ();

  g_assert_no_error (error);
  g_assert_cmpstr (identity.request_id, ==, "postfix:queue-1:delivery-1");
  g_assert_nonnull (request.message_bytes);

  report.elapsed_us = end_us - start_us;
  wyrebox_benchmark_report_capture_rusage (&report);
  report.object_count = 1;
  report.disk_bytes = g_bytes_get_size (request.message_bytes);

  wyrebox_benchmark_report_print_json ("postfix-pipe", "input-build", &report);
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  run_microbenchmark ();

  return 0;
}
