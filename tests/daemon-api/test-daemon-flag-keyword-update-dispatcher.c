#include "wyrebox-daemon-flag-keyword-update-dispatcher.h"

#include <gio/gio.h>
#include <glib.h>

static void
fill_success_receipt (WyreboxDaemonSuccessReceipt *receipt,
    const char *request_id)
{
  receipt->request_id = g_strdup (request_id);
  receipt->durable_marker = g_strdup ("journal:123:456");
  receipt->journal_offset = 123;
  receipt->journal_sequence = 456;
  receipt->summary = g_strdup ("flag_keyword_update mode=set");
}

static gboolean
update_flags_fixture (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFlagKeywordUpdateRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt,
    gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;

  g_assert_cmpstr (identity->request_id, ==, "request-flags");
  g_assert_cmpstr (identity->caller_identity, ==, "dovecot");
  g_assert_cmpstr (identity->account_identity, ==, "account-1");
  g_assert_cmpstr (identity->tool_identity, ==, "dovecot-storage");
  g_assert_cmpstr (request->account_identity, ==, "account-1");
  g_assert_cmpstr (request->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpuint (request->uid_validity, ==, 77);
  g_assert_cmpuint (request->mailbox_uid, ==, 42);
  g_assert_cmpint (request->mode, ==,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET);
  g_assert_cmpstr (request->system_flags[0], ==, "\\Seen");
  g_assert_cmpstr (request->user_keywords[0], ==, "project");

  if (was_called != NULL)
    *was_called = TRUE;

  fill_success_receipt (out_receipt, identity->request_id);
  return TRUE;
}

static gboolean
fail_update_without_error (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFlagKeywordUpdateRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt,
    gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;

  (void) identity;
  (void) request;
  (void) out_receipt;

  *was_called = TRUE;
  return FALSE;
}

static gboolean
update_with_mismatched_receipt (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFlagKeywordUpdateRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt,
    gpointer user_data, GError **error)
{
  gboolean *was_called = user_data;

  (void) identity;
  (void) request;

  *was_called = TRUE;
  fill_success_receipt (out_receipt, "request-other");
  return TRUE;
}

static gboolean
init_update_request (WyreboxDaemonFlagKeywordUpdateRequest *request,
    const char *account_identity, GError **error)
{
  const char *system_flags[] = { "\\Seen", NULL };
  const char *user_keywords[] = { "project", NULL };

  return wyrebox_daemon_flag_keyword_update_request_init (request,
      account_identity,
      "mailbox-inbox",
      77,
      42,
      WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
      system_flags, user_keywords, error);
}

static void
test_dispatcher_handles_valid_envelope (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonFlagKeywordUpdateService) service = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (init_update_request (&request, "account-1", &error));
  g_assert_no_error (error);

  service =
      wyrebox_daemon_flag_keyword_update_service_new (update_flags_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_flag_keyword_update_dispatch (service,
          "request-flags",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-store-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
  g_assert_cmpstr (frame.request_id, ==, "request-flags");
  g_assert_cmpstr (frame.correlation_id, ==, "imap-store-1");
  g_assert_cmpstr (frame.success.durable_marker, ==, "journal:123:456");
}

static void
test_dispatcher_rejects_unauthorized_caller_with_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonFlagKeywordUpdateService) service = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (init_update_request (&request, "account-1", &error));
  g_assert_no_error (error);

  service =
      wyrebox_daemon_flag_keyword_update_service_new (update_flags_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_flag_keyword_update_dispatch (service,
          "request-flags",
          "skill",
          "account-1",
          "fact-importer", "imap-store-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_dispatcher_rejects_account_mismatch_with_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonFlagKeywordUpdateService) service = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (init_update_request (&request, "account-2", &error));
  g_assert_no_error (error);

  service =
      wyrebox_daemon_flag_keyword_update_service_new (update_flags_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_flag_keyword_update_dispatch (service,
          "request-flags",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-store-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

static void
test_dispatcher_rejects_missing_request_id_before_service (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonFlagKeywordUpdateService) service = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (init_update_request (&request, "account-1", &error));
  g_assert_no_error (error);

  service =
      wyrebox_daemon_flag_keyword_update_service_new (update_flags_fixture,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_false (wyrebox_daemon_flag_keyword_update_dispatch (service,
          "",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-store-1", &request, &frame, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_false (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_NONE);
}

static void
test_dispatcher_converts_silent_failure_to_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonFlagKeywordUpdateService) service = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (init_update_request (&request, "account-1", &error));
  g_assert_no_error (error);

  service =
      wyrebox_daemon_flag_keyword_update_service_new (fail_update_without_error,
      &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_flag_keyword_update_dispatch (service,
          "request-flags",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-store-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_INTERNAL_ERROR);
}

static void
test_dispatcher_rejects_mismatched_receipt_with_error_frame (void)
{
  gboolean was_called = FALSE;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonFlagKeywordUpdateService) service = NULL;
  g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

  g_assert_true (init_update_request (&request, "account-1", &error));
  g_assert_no_error (error);

  service =
      wyrebox_daemon_flag_keyword_update_service_new
      (update_with_mismatched_receipt, &was_called, NULL);
  g_assert_nonnull (service);

  g_assert_true (wyrebox_daemon_flag_keyword_update_dispatch (service,
          "request-flags",
          "dovecot",
          "account-1",
          "dovecot-storage", "imap-store-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_true (was_called);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/flag-keyword-update-dispatcher/"
      "handles-valid-envelope", test_dispatcher_handles_valid_envelope);
  g_test_add_func ("/daemon-api/flag-keyword-update-dispatcher/"
      "rejects-unauthorized-caller-with-error-frame",
      test_dispatcher_rejects_unauthorized_caller_with_error_frame);
  g_test_add_func ("/daemon-api/flag-keyword-update-dispatcher/"
      "rejects-account-mismatch-with-error-frame",
      test_dispatcher_rejects_account_mismatch_with_error_frame);
  g_test_add_func ("/daemon-api/flag-keyword-update-dispatcher/"
      "rejects-missing-request-id-before-service",
      test_dispatcher_rejects_missing_request_id_before_service);
  g_test_add_func ("/daemon-api/flag-keyword-update-dispatcher/"
      "converts-silent-failure-to-error-frame",
      test_dispatcher_converts_silent_failure_to_error_frame);
  g_test_add_func ("/daemon-api/flag-keyword-update-dispatcher/"
      "rejects-mismatched-receipt-with-error-frame",
      test_dispatcher_rejects_mismatched_receipt_with_error_frame);

  return g_test_run ();
}
