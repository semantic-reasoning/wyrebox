#include "wyrebox-daemon-mail-event-stream-request.h"

#include <gio/gio.h>

static void
test_request_init_copies_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-1", "mailbox-1", "message-1", "MessageDelivered",
          4096, 7, 1718760000000000ULL, 1718769999999999ULL, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.account_identity, ==, "account-1");
  g_assert_cmpstr (request.mailbox_identity, ==, "mailbox-1");
  g_assert_cmpstr (request.message_id, ==, "message-1");
  g_assert_cmpstr (request.event_type, ==, "MessageDelivered");
  g_assert_cmpuint (request.after_journal_offset, ==, 4096);
  g_assert_cmpuint (request.after_journal_sequence, ==, 7);
  g_assert_cmpuint (request.after_unix_us, ==, 1718760000000000ULL);
  g_assert_cmpuint (request.before_unix_us, ==, 1718769999999999ULL);
}

static void
test_request_reinitializes (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-1", "mailbox-1", "message-1", "MessageDelivered",
          4096, 7, 1718760000000000ULL, 1718769999999999ULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-2", "mailbox-2", "message-2", "FlagChanged",
          8192, 11, 1718770000000000ULL, 1718779999999999ULL, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.account_identity, ==, "account-2");
  g_assert_cmpstr (request.mailbox_identity, ==, "mailbox-2");
  g_assert_cmpstr (request.message_id, ==, "message-2");
  g_assert_cmpstr (request.event_type, ==, "FlagChanged");
  g_assert_cmpuint (request.after_journal_offset, ==, 8192);
  g_assert_cmpuint (request.after_journal_sequence, ==, 11);
  g_assert_cmpuint (request.after_unix_us, ==, 1718770000000000ULL);
  g_assert_cmpuint (request.before_unix_us, ==, 1718779999999999ULL);
}

static void
test_request_rejects_missing_account_identity (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mail_event_stream_request_init (&request,
          "", NULL, NULL, NULL, 4096, 7, 0, 0, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_request_rejects_control_characters_in_account_identity (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-1\n", NULL, NULL, NULL, 4096, 7, 0, 0, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_request_accepts_zero_cursor (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-1", NULL, NULL, NULL, 0, 0, 0, 0, &error));
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
          "account-1", NULL, NULL, NULL, 0, 7, 0, 0, &error));
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
          "account-1", NULL, NULL, NULL, 4096, 0, 0, 0, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_request_rejects_inverted_time_range (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailEventStreamRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mail_event_stream_request_init (&request,
          "account-1", NULL, NULL, NULL, 0, 0,
          1718779999999999ULL, 1718760000000000ULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
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
  g_test_add_func ("/daemon-api/mail-event-stream-request/"
      "rejects-inverted-time-range", test_request_rejects_inverted_time_range);

  return g_test_run ();
}
