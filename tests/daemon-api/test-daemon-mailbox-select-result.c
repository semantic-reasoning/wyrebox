#include "wyrebox-daemon-mailbox-select-result.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_mailbox_select_result_copies_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
  char mailbox_id[] = "mailbox-inbox";
  char mailbox_name[] = "INBOX";

  g_assert_true (wyrebox_daemon_mailbox_select_result_init (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          mailbox_id, mailbox_name, 77, 42, &error));
  g_assert_no_error (error);

  mailbox_id[0] = 'X';
  mailbox_name[0] = 'X';

  g_assert_cmpint (result.kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  g_assert_cmpstr (result.mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpstr (result.mailbox_name, ==, "INBOX");
  g_assert_cmpuint (result.uid_validity, ==, 77);
  g_assert_cmpuint (result.uid_next, ==, 42);
}

static void
test_mailbox_select_result_represents_virtual_mailbox (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_result_init (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-project-a", "Projects/Project A", 99, 1, &error));
  g_assert_no_error (error);

  g_assert_cmpint (result.kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (result.mailbox_id, ==, "view-project-a");
  g_assert_cmpstr (result.mailbox_name, ==, "Projects/Project A");
  g_assert_cmpuint (result.uid_validity, ==, 99);
  g_assert_cmpuint (result.uid_next, ==, 1);
}

static void
test_mailbox_select_result_reinitializes (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_result_init (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", 77, 42, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_mailbox_select_result_init (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-project-a", "Projects/Project A", 99, 1, &error));
  g_assert_no_error (error);

  g_assert_cmpint (result.kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (result.mailbox_id, ==, "view-project-a");
  g_assert_cmpstr (result.mailbox_name, ==, "Projects/Project A");
}

static void
test_mailbox_select_result_rejects_invalid_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };

  g_assert_false (wyrebox_daemon_mailbox_select_result_init (&result,
          (WyreboxDaemonMailboxListEntryKind) 99,
          "mailbox-inbox", "INBOX", 77, 42, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (result.mailbox_id);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_mailbox_select_result_init (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "", "INBOX", 77, 42, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (result.mailbox_id);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_mailbox_select_result_init (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX\n", 77, 42, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (result.mailbox_name);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_mailbox_select_result_init (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", 0, 42, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_mailbox_select_result_init (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", 77, 0, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_mailbox_select_result_failure_leaves_existing_contents (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_result_init (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", 77, 42, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_mailbox_select_result_init (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "", "Broken", 1, 1, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_assert_cmpint (result.kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  g_assert_cmpstr (result.mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpstr (result.mailbox_name, ==, "INBOX");
  g_assert_cmpuint (result.uid_validity, ==, 77);
  g_assert_cmpuint (result.uid_next, ==, 42);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/mailbox-select-result/copies-fields",
      test_mailbox_select_result_copies_fields);
  g_test_add_func ("/daemon-api/mailbox-select-result/"
      "represents-virtual-mailbox",
      test_mailbox_select_result_represents_virtual_mailbox);
  g_test_add_func ("/daemon-api/mailbox-select-result/reinitializes",
      test_mailbox_select_result_reinitializes);
  g_test_add_func ("/daemon-api/mailbox-select-result/rejects-invalid-fields",
      test_mailbox_select_result_rejects_invalid_fields);
  g_test_add_func ("/daemon-api/mailbox-select-result/"
      "failure-leaves-existing-contents",
      test_mailbox_select_result_failure_leaves_existing_contents);

  return g_test_run ();
}
