#include "wyrebox-daemon-mailbox-list-request.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_mailbox_list_request_copies_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListRequest) request = { 0 };
  char account[] = "account-1";
  char prefix[] = "Projects/";

  g_assert_true (wyrebox_daemon_mailbox_list_request_init (&request,
          account, prefix, &error));
  g_assert_no_error (error);

  account[0] = 'X';
  prefix[0] = 'X';

  g_assert_cmpstr (request.account_identity, ==, "account-1");
  g_assert_cmpstr (request.namespace_prefix, ==, "Projects/");
}

static void
test_mailbox_list_request_defaults_missing_prefix_to_root (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_list_request_init (&request,
          "account-1", NULL, &error));
  g_assert_no_error (error);
  g_assert_cmpstr (request.namespace_prefix, ==, "");
}

static void
test_mailbox_list_request_reinitializes (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListRequest) request = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_list_request_init (&request,
          "account-1", "Archive/", &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_list_request_init (&request,
          "account-2", "", &error));
  g_assert_no_error (error);

  g_assert_cmpstr (request.account_identity, ==, "account-2");
  g_assert_cmpstr (request.namespace_prefix, ==, "");
}

static void
test_mailbox_list_request_rejects_missing_account (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mailbox_list_request_init (&request,
          "", "", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.account_identity);
}

static void
test_mailbox_list_request_rejects_control_characters (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListRequest) request = { 0 };

  g_assert_false (wyrebox_daemon_mailbox_list_request_init (&request,
          "account-1", "Projects\n", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (request.namespace_prefix);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/mailbox-list-request/copies-fields",
      test_mailbox_list_request_copies_fields);
  g_test_add_func ("/daemon-api/mailbox-list-request/"
      "defaults-missing-prefix-to-root",
      test_mailbox_list_request_defaults_missing_prefix_to_root);
  g_test_add_func ("/daemon-api/mailbox-list-request/reinitializes",
      test_mailbox_list_request_reinitializes);
  g_test_add_func ("/daemon-api/mailbox-list-request/rejects-missing-account",
      test_mailbox_list_request_rejects_missing_account);
  g_test_add_func ("/daemon-api/mailbox-list-request/"
      "rejects-control-characters",
      test_mailbox_list_request_rejects_control_characters);

  return g_test_run ();
}
