#include "wyrebox-daemon-message-search-request.h"

#include <gio/gio.h>

static void
test_message_search_request_copies_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "mailbox-inbox", 77, "unseen", &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.account_identity, ==, "account-1");
  g_assert_cmpstr (request.mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpuint (request.uid_validity, ==, 77);
  g_assert_cmpstr (request.criteria_token, ==, "unseen");
}

static void
test_message_search_request_reinitializes (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "mailbox-inbox", 77, "unseen", &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_message_search_request_init (&request,
          "account-2", "mailbox-other", 99, "answered", &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.account_identity, ==, "account-2");
  g_assert_cmpstr (request.mailbox_id, ==, "mailbox-other");
  g_assert_cmpuint (request.uid_validity, ==, 99);
  g_assert_cmpstr (request.criteria_token, ==, "answered");
}

static void
test_message_search_request_rejects_missing_account (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_message_search_request_init (&request,
          NULL, "mailbox-inbox", 77, "unseen", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_message_search_request_rejects_missing_mailbox_id (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "", 77, "unseen", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.mailbox_id);
}

static void
test_message_search_request_rejects_zero_uid_validity (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "mailbox-inbox", 0, "unseen", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (request.uid_validity, ==, 0);
}

static void
test_message_search_request_rejects_empty_criteria_token (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "mailbox-inbox", 77, "", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.criteria_token);
}

static void
test_message_search_request_rejects_control_character_text (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "mailbox-inbox", 77, "in\nbox", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.criteria_token);
}

static void
test_message_search_request_rejects_backslash_criteria_token (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "mailbox-inbox", 77, "in\\box", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.criteria_token);
}

static void
test_message_search_request_rejects_criteria_sql_chars (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_message_search_request_init (&request,
          "account-1", "mailbox-inbox", 77, "FROM \"me\" OR", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.criteria_token);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/message-search-request/copies-fields",
      test_message_search_request_copies_fields);
  g_test_add_func ("/daemon-api/message-search-request/"
      "reinitializes", test_message_search_request_reinitializes);
  g_test_add_func ("/daemon-api/message-search-request/"
      "rejects-missing-account",
      test_message_search_request_rejects_missing_account);
  g_test_add_func ("/daemon-api/message-search-request/"
      "rejects-missing-mailbox-id",
      test_message_search_request_rejects_missing_mailbox_id);
  g_test_add_func ("/daemon-api/message-search-request/"
      "rejects-zero-uid-validity",
      test_message_search_request_rejects_zero_uid_validity);
  g_test_add_func ("/daemon-api/message-search-request/"
      "rejects-empty-criteria-token",
      test_message_search_request_rejects_empty_criteria_token);
  g_test_add_func ("/daemon-api/message-search-request/"
      "rejects-control-characters",
      test_message_search_request_rejects_control_character_text);
  g_test_add_func ("/daemon-api/message-search-request/"
      "rejects-backslash-criteria-token",
      test_message_search_request_rejects_backslash_criteria_token);
  g_test_add_func ("/daemon-api/message-search-request/"
      "rejects-criteria-token-with-sql-characters",
      test_message_search_request_rejects_criteria_sql_chars);

  return g_test_run ();
}
