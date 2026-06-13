#include "wyrebox-daemon-mailbox-select-dispatcher.h"

#include <gio/gio.h>
#include <glib.h>

static gboolean
select_fixture_mailbox (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxSelectRequest *request,
    WyreboxDaemonMailboxSelectResult *out_result,
    gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;

  g_assert_cmpstr (identity->request_id, ==, "request-select");
  g_assert_cmpstr (identity->caller_identity, ==, "dovecot");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (request->account_identity, ==, "account-1");
  g_assert_cmpstr (request->mailbox_id, ==, "mailbox-inbox");
  g_assert_null (request->mailbox_name);

  *was_called = TRUE;
  return wyrebox_daemon_mailbox_select_result_init (out_result,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
      "mailbox-inbox", "INBOX", 77, 42, 123, error);
}

static gboolean
fail_select_without_error (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMailboxSelectRequest *request,
    WyreboxDaemonMailboxSelectResult *out_result,
    gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;

  *was_called = TRUE;
  return FALSE;
}

static void
test_mailbox_select_dispatcher_handles_valid_envelope (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxSelectService) service = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", "mailbox-inbox", NULL, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_mailbox_select_service_new (select_fixture_mailbox,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_mailbox_select_dispatch (service,
          "request-select",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-select-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==,
      WYREBOX_DAEMON_RESPONSE_FRAME_MAILBOX_SELECT);
  g_assert_cmpstr (frame.request_id, ==, "request-select");
  g_assert_cmpstr (frame.correlation_id, ==, "imap-select-1");
  g_assert_cmpstr (frame.mailbox_select.mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpstr (frame.mailbox_select.mailbox_name, ==, "INBOX");
  g_assert_cmpuint (frame.mailbox_select.uid_validity, ==, 77);
  g_assert_cmpuint (frame.mailbox_select.uid_next, ==, 42);
  g_assert_cmpuint (frame.mailbox_select.message_count, ==, 123);
}

static void
    test_mailbox_select_dispatcher_rejects_unauthorized_caller_with_error_frame
    (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxSelectService) service = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", "mailbox-inbox", NULL, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_mailbox_select_service_new (select_fixture_mailbox,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_mailbox_select_dispatch (service,
          "request-select",
          "skill",
          "account-1",
          "fact-importer", "imap-select-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_mailbox_select_dispatcher_rejects_account_mismatch_with_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxSelectService) service = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-2", "mailbox-inbox", NULL, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_mailbox_select_service_new (select_fixture_mailbox,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_mailbox_select_dispatch (service,
          "request-select",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-select-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_mailbox_select_dispatcher_rejects_missing_request_id_before_service (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxSelectService) service = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", "mailbox-inbox", NULL, &error));
  g_assert_no_error (error);

  service = wyrebox_daemon_mailbox_select_service_new (select_fixture_mailbox,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_mailbox_select_dispatch (service,
          "",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-select-1", &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
}

static void
test_mailbox_select_dispatcher_converts_silent_failure_to_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonMailboxSelectService) service = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", "mailbox-inbox", NULL, &error));
  g_assert_no_error (error);

  service =
      wyrebox_daemon_mailbox_select_service_new (fail_select_without_error,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_mailbox_select_dispatch (service,
          "request-select",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-select-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/mailbox-select-dispatcher/"
      "handles-valid-envelope",
      test_mailbox_select_dispatcher_handles_valid_envelope);
  g_test_add_func ("/daemon-api/mailbox-select-dispatcher/"
      "rejects-unauthorized-caller-with-error-frame",
      test_mailbox_select_dispatcher_rejects_unauthorized_caller_with_error_frame);
  g_test_add_func ("/daemon-api/mailbox-select-dispatcher/"
      "rejects-account-mismatch-with-error-frame",
      test_mailbox_select_dispatcher_rejects_account_mismatch_with_error_frame);
  g_test_add_func ("/daemon-api/mailbox-select-dispatcher/"
      "rejects-missing-request-id-before-service",
      test_mailbox_select_dispatcher_rejects_missing_request_id_before_service);
  g_test_add_func ("/daemon-api/mailbox-select-dispatcher/"
      "converts-silent-failure-to-error-frame",
      test_mailbox_select_dispatcher_converts_silent_failure_to_error_frame);

  return g_test_run ();
}
