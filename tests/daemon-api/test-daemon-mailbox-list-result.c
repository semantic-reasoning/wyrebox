#include "wyrebox-daemon-mailbox-list-result.h"

#include <gio/gio.h>
#include <glib.h>

static void
test_mailbox_list_result_allows_empty_result (void)
{
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };

  wyrebox_daemon_mailbox_list_result_init_empty (&result);

  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries (&result),
      ==, 0);
}

static void
test_mailbox_list_result_appends_inbox_entry (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  const WyreboxDaemonMailboxListEntry *entry = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          "mailbox-inbox", "INBOX", "\\Inbox", TRUE, FALSE, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries (&result),
      ==, 1);
  entry = wyrebox_daemon_mailbox_list_result_get_entry (&result, 0);
  g_assert_nonnull (entry);
  g_assert_cmpstr (entry->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpstr (entry->mailbox_name, ==, "INBOX");
  g_assert_cmpstr (entry->special_use, ==, "\\Inbox");
  g_assert_true (entry->is_selectable);
  g_assert_false (entry->is_virtual);
}

static void
test_mailbox_list_result_deep_copies_entry_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  const WyreboxDaemonMailboxListEntry *entry = NULL;
  char mailbox_id[] = "mailbox-1";
  char mailbox_name[] = "Projects";
  char special_use[] = "\\Archive";

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          mailbox_id, mailbox_name, special_use, TRUE, FALSE, &error));
  g_assert_no_error (error);

  mailbox_id[0] = 'X';
  mailbox_name[0] = 'X';
  special_use[1] = 'X';

  entry = wyrebox_daemon_mailbox_list_result_get_entry (&result, 0);
  g_assert_cmpstr (entry->mailbox_id, ==, "mailbox-1");
  g_assert_cmpstr (entry->mailbox_name, ==, "Projects");
  g_assert_cmpstr (entry->special_use, ==, "\\Archive");
}

static void
test_mailbox_list_result_represents_virtual_mailbox (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  const WyreboxDaemonMailboxListEntry *entry = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          "view-project-a", "Projects/Project A", NULL, TRUE, TRUE, &error));
  g_assert_no_error (error);

  entry = wyrebox_daemon_mailbox_list_result_get_entry (&result, 0);
  g_assert_nonnull (entry);
  g_assert_cmpstr (entry->special_use, ==, NULL);
  g_assert_true (entry->is_selectable);
  g_assert_true (entry->is_virtual);
}

static void
test_mailbox_list_result_rejects_invalid_entry_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_false (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          "", "INBOX", NULL, TRUE, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries (&result),
      ==, 0);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          "mailbox-inbox", "INBOX\n", NULL, TRUE, FALSE, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries (&result),
      ==, 0);
}

static void
test_mailbox_list_entry_reinitializes (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListEntry) entry = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_list_entry_init (&entry,
          "mailbox-1", "Archive", "\\Archive", TRUE, FALSE, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_list_entry_init (&entry,
          "view-1", "Smart/Invoices", NULL, FALSE, TRUE, &error));
  g_assert_no_error (error);

  g_assert_cmpstr (entry.mailbox_id, ==, "view-1");
  g_assert_cmpstr (entry.mailbox_name, ==, "Smart/Invoices");
  g_assert_cmpstr (entry.special_use, ==, NULL);
  g_assert_false (entry.is_selectable);
  g_assert_true (entry.is_virtual);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/daemon-api/mailbox-list-result/allows-empty-result",
      test_mailbox_list_result_allows_empty_result);
  g_test_add_func ("/daemon-api/mailbox-list-result/appends-inbox-entry",
      test_mailbox_list_result_appends_inbox_entry);
  g_test_add_func ("/daemon-api/mailbox-list-result/deep-copies-entry-fields",
      test_mailbox_list_result_deep_copies_entry_fields);
  g_test_add_func ("/daemon-api/mailbox-list-result/represents-virtual-mailbox",
      test_mailbox_list_result_represents_virtual_mailbox);
  g_test_add_func ("/daemon-api/mailbox-list-result/"
      "rejects-invalid-entry-fields",
      test_mailbox_list_result_rejects_invalid_entry_fields);
  g_test_add_func ("/daemon-api/mailbox-list-result/entry-reinitializes",
      test_mailbox_list_entry_reinitializes);

  return g_test_run ();
}
