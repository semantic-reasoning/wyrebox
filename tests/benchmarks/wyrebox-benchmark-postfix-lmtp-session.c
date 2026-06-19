#include "wyrebox-benchmark-report.h"
#include "wyrebox-postfix-lmtp-session.h"

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
  static const guint8 transcript[] =
      "LHLO postfix.example\r\n"
      "MAIL FROM:<sender@example.com>\r\n"
      "RCPT TO:<recipient@example.com>\r\n"
      "DATA\r\n"
      "Subject: wyrebox postfix lmtp benchmark\r\n"
      "\r\n" "fixed RFC 5322 payload\r\n" ".\r\n";
  static const char *argv[] = {
    "wyrebox-postfix-lmtp",
    "--account-id", "account-1",
    "--delivery-id", "delivery-1",
    "--socket", "/run/wyrebox/wyrebox.sock",
    NULL
  };
  g_auto (WyreboxBenchmarkReport) report = { 0 };
  g_auto (WyreboxPostfixLmtpOptions) options = { 0 };
  g_auto (WyreboxDaemonRequestIdentity) identity = { 0 };
  g_auto (WyreboxDaemonDeliveryIngestionRequest) request = { 0 };
  g_autoptr (GInputStream) input = NULL;
  g_autoptr (GError) error = NULL;
  gint64 start_us = 0;
  gint64 end_us = 0;
  int argc = 0;

  while (argv[argc] != NULL)
    argc++;

  input = stream_from_data (transcript, sizeof (transcript) - 1);
  g_assert_nonnull (input);

  wyrebox_benchmark_report_init (&report);

  start_us = g_get_monotonic_time ();
  g_assert_true (wyrebox_postfix_lmtp_session_build (argc, argv,
          input, 4096, &options, &identity, &request, &error));
  end_us = g_get_monotonic_time ();

  g_assert_no_error (error);
  g_assert_cmpstr (identity.request_id, ==, "postfix-lmtp:delivery-1");
  g_assert_cmpstr (options.socket_path, ==, "/run/wyrebox/wyrebox.sock");
  g_assert_nonnull (request.message_bytes);

  report.elapsed_us = end_us - start_us;
  wyrebox_benchmark_report_capture_rusage (&report);
  report.object_count = 1;
  report.disk_bytes = g_bytes_get_size (request.message_bytes);

  wyrebox_benchmark_report_print_json ("postfix-lmtp",
      "session-build", &report);
}

int
main (int argc, char **argv)
{
  (void) argc;
  (void) argv;

  run_microbenchmark ();

  return 0;
}
