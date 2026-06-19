#include "wyrebox-daemon-mailbox-status-request.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_mailbox_status_request_copies_mailbox_id_selector (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxStatusRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_status_request_init (&request,
          "account-1", "mailbox-inbox", NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (request.account_identity, ==, "account-1");
  g_assert_cmpstr (request.mailbox_id, ==, "mailbox-inbox");
  g_assert_null (request.mailbox_name);
}

static void
test_mailbox_status_request_copies_mailbox_name_selector (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxStatusRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_status_request_init (&request,
          "account-1", NULL, "INBOX", &error));
  g_assert_no_error (error);
  g_assert_cmpstr (request.account_identity, ==, "account-1");
  g_assert_null (request.mailbox_id);
  g_assert_cmpstr (request.mailbox_name, ==, "INBOX");
}

static void
test_mailbox_status_request_reinitializes (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxStatusRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_status_request_init (&request,
          "account-1", "mailbox-a", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_status_request_init (&request,
          "account-2", NULL, "INBOX", &error));
  g_assert_no_error (error);
  g_assert_cmpstr (request.account_identity, ==, "account-2");
  g_assert_null (request.mailbox_id);
  g_assert_cmpstr (request.mailbox_name, ==, "INBOX");
}

static void
test_mailbox_status_request_rejects_missing_account (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxStatusRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mailbox_status_request_init (&request,
          NULL, "mailbox-inbox", NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_mailbox_status_request_requires_exactly_one_selector (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxStatusRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mailbox_status_request_init (&request,
          "account-1", NULL, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_mailbox_status_request_init (&request,
          "account-1", "mailbox-inbox", "INBOX", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_mailbox_status_request_rejects_control_characters (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxStatusRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mailbox_status_request_init (&request,
          "account-1", "mailbox-\n-inbox", NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/mailbox-status-request/"
      "copies-mailbox-id-selector",
      test_mailbox_status_request_copies_mailbox_id_selector);
  g_test_add_func ("/daemon-api/mailbox-status-request/"
      "copies-mailbox-name-selector",
      test_mailbox_status_request_copies_mailbox_name_selector);
  g_test_add_func ("/daemon-api/mailbox-status-request/"
      "reinitializes", test_mailbox_status_request_reinitializes);
  g_test_add_func ("/daemon-api/mailbox-status-request/"
      "rejects-missing-account",
      test_mailbox_status_request_rejects_missing_account);
  g_test_add_func ("/daemon-api/mailbox-status-request/"
      "requires-exactly-one-selector",
      test_mailbox_status_request_requires_exactly_one_selector);
  g_test_add_func ("/daemon-api/mailbox-status-request/"
      "rejects-control-characters",
      test_mailbox_status_request_rejects_control_characters);

  return g_test_run ();
}
