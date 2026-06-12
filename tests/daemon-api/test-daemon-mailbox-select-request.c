#include "wyrebox-daemon-mailbox-select-request.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_mailbox_select_request_copies_mailbox_id_selector (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  char account[] = "account-1";
  char mailbox_id[] = "mailbox-inbox";

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          account, mailbox_id, NULL, &error));
  g_assert_no_error (error);

  account[0] = 'X';
  mailbox_id[0] = 'X';

  g_assert_cmpstr (request.account_identity, ==, "account-1");
  g_assert_cmpstr (request.mailbox_id, ==, "mailbox-inbox");
  g_assert_null (request.mailbox_name);
}

static void
test_mailbox_select_request_copies_mailbox_name_selector (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  char mailbox_name[] = "Projects/Project A";

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", NULL, mailbox_name, &error));
  g_assert_no_error (error);

  mailbox_name[0] = 'X';

  g_assert_cmpstr (request.account_identity, ==, "account-1");
  g_assert_null (request.mailbox_id);
  g_assert_cmpstr (request.mailbox_name, ==, "Projects/Project A");
}

static void
test_mailbox_select_request_reinitializes (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", "mailbox-inbox", NULL, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-2", NULL, "Archive", &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.account_identity, ==, "account-2");
  g_assert_null (request.mailbox_id);
  g_assert_cmpstr (request.mailbox_name, ==, "Archive");
}

static void
test_mailbox_select_request_rejects_missing_account (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mailbox_select_request_init (&request,
          "", "mailbox-inbox", NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_mailbox_select_request_requires_exactly_one_selector (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", NULL, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.mailbox_id);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", "mailbox-inbox", "INBOX", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.mailbox_name);
}

static void
test_mailbox_select_request_rejects_control_characters (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", NULL, "Projects\n", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.mailbox_name);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/mailbox-select-request/"
      "copies-mailbox-id-selector",
      test_mailbox_select_request_copies_mailbox_id_selector);
  g_test_add_func ("/daemon-api/mailbox-select-request/"
      "copies-mailbox-name-selector",
      test_mailbox_select_request_copies_mailbox_name_selector);
  g_test_add_func ("/daemon-api/mailbox-select-request/reinitializes",
      test_mailbox_select_request_reinitializes);
  g_test_add_func ("/daemon-api/mailbox-select-request/rejects-missing-account",
      test_mailbox_select_request_rejects_missing_account);
  g_test_add_func ("/daemon-api/mailbox-select-request/"
      "requires-exactly-one-selector",
      test_mailbox_select_request_requires_exactly_one_selector);
  g_test_add_func ("/daemon-api/mailbox-select-request/"
      "rejects-control-characters",
      test_mailbox_select_request_rejects_control_characters);

  return g_test_run ();
}
