#include "wyrebox-daemon-message-fetch-request.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_message_fetch_request_copies_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox", 77, 42, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.account_identity, ==, "account-1");
  g_assert_cmpstr (request.mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpuint (request.uid_validity, ==, 77);
  g_assert_cmpuint (request.mailbox_uid, ==, 42);
}

static void
test_message_fetch_request_reinitializes (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox", 77, 42, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_message_fetch_request_init (&request,
          "account-2", "mailbox-other", 99, 100, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.account_identity, ==, "account-2");
  g_assert_cmpstr (request.mailbox_id, ==, "mailbox-other");
  g_assert_cmpuint (request.uid_validity, ==, 99);
  g_assert_cmpuint (request.mailbox_uid, ==, 100);
}

static void
test_message_fetch_request_rejects_missing_account (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_message_fetch_request_init (&request,
          "", "mailbox-inbox", 77, 42, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_message_fetch_request_rejects_missing_mailbox_id (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "", 77, 42, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.mailbox_id);
}

static void
test_message_fetch_request_rejects_zero_uid_validity (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox", 0, 42, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_message_fetch_request_rejects_zero_mailbox_uid (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_message_fetch_request_init (&request,
          "account-1", "mailbox-inbox", 77, 0, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_message_fetch_request_rejects_control_characters (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMessageFetchRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_message_fetch_request_init (&request,
          "account\n1", "mailbox-inbox", 77, 42, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/message-fetch-request/copies-fields",
      test_message_fetch_request_copies_fields);
  g_test_add_func ("/daemon-api/message-fetch-request/"
      "reinitializes", test_message_fetch_request_reinitializes);
  g_test_add_func ("/daemon-api/message-fetch-request/"
      "rejects-missing-account",
      test_message_fetch_request_rejects_missing_account);
  g_test_add_func ("/daemon-api/message-fetch-request/"
      "rejects-missing-mailbox-id",
      test_message_fetch_request_rejects_missing_mailbox_id);
  g_test_add_func ("/daemon-api/message-fetch-request/"
      "rejects-zero-uid-validity",
      test_message_fetch_request_rejects_zero_uid_validity);
  g_test_add_func ("/daemon-api/message-fetch-request/"
      "rejects-zero-mailbox-uid",
      test_message_fetch_request_rejects_zero_mailbox_uid);
  g_test_add_func ("/daemon-api/message-fetch-request/"
      "rejects-control-characters",
      test_message_fetch_request_rejects_control_characters);

  return g_test_run ();
}
