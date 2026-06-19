#include "wyrebox-daemon-mail-event-stream-request.h"

#include <gio/gio.h>

static void
test_request_init_copies_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-1", 4096, 7, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.account_identity, ==, "account-1");
  g_assert_cmpuint (request.after_journal_offset, ==, 4096);
  g_assert_cmpuint (request.after_journal_sequence, ==, 7);
}

static void
test_request_reinitializes (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-1", 4096, 7, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-2", 8192, 11, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.account_identity, ==, "account-2");
  g_assert_cmpuint (request.after_journal_offset, ==, 8192);
  g_assert_cmpuint (request.after_journal_sequence, ==, 11);
}

static void
test_request_rejects_missing_account_identity (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mail_event_stream_request_init (&request,
          "", 4096, 7, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_request_rejects_control_characters_in_account_identity (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-1\n", 4096, 7, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_request_accepts_zero_cursor (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-1", 0, 0, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (request.account_identity, ==, "account-1");
  g_assert_cmpuint (request.after_journal_offset, ==, 0);
  g_assert_cmpuint (request.after_journal_sequence, ==, 0);
}

static void
test_request_accepts_first_record_cursor (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-1", 0, 7, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (request.account_identity, ==, "account-1");
  g_assert_cmpuint (request.after_journal_offset, ==, 0);
  g_assert_cmpuint (request.after_journal_sequence, ==, 7);
}

static void
test_request_rejects_half_open_cursor_offset_only (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-1", 4096, 0, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/mail-event-stream-request/copies-fields",
      test_request_init_copies_fields);
  g_test_add_func ("/daemon-api/mail-event-stream-request/reinitializes",
      test_request_reinitializes);
  g_test_add_func ("/daemon-api/mail-event-stream-request/"
      "rejects-missing-account-identity",
      test_request_rejects_missing_account_identity);
  g_test_add_func ("/daemon-api/mail-event-stream-request/"
      "rejects-control-characters-in-account-identity",
      test_request_rejects_control_characters_in_account_identity);
  g_test_add_func ("/daemon-api/mail-event-stream-request/"
      "accepts-zero-cursor", test_request_accepts_zero_cursor);
  g_test_add_func ("/daemon-api/mail-event-stream-request/"
      "accepts-first-record-cursor", test_request_accepts_first_record_cursor);
  g_test_add_func ("/daemon-api/mail-event-stream-request/"
      "rejects-half-open-cursor-offset-only",
      test_request_rejects_half_open_cursor_offset_only);

  return g_test_run ();
}
