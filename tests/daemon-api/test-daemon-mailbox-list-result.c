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
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", "/", "\\Inbox", TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries (&result),
      ==, 1);
  entry = wyrebox_daemon_mailbox_list_result_get_entry (&result, 0);
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  g_assert_cmpstr (entry->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpstr (entry->mailbox_name, ==, "INBOX");
  g_assert_cmpstr (entry->hierarchy_delimiter, ==, "/");
  g_assert_cmpstr (entry->special_use, ==, "\\Inbox");
  g_assert_true (entry->is_selectable);
  g_assert_cmpint (entry->child_state, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN);
}

static void
test_mailbox_list_result_deep_copies_entry_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  const WyreboxDaemonMailboxListEntry *entry = NULL;
  char mailbox_id[] = "mailbox-1";
  char mailbox_name[] = "Projects";
  char hierarchy_delimiter[] = "/";
  char special_use[] = "\\Archive";

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          mailbox_id, mailbox_name, hierarchy_delimiter, special_use, TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN, &error));
  g_assert_no_error (error);

  mailbox_id[0] = 'X';
  mailbox_name[0] = 'X';
  hierarchy_delimiter[0] = '.';
  special_use[1] = 'X';

  entry = wyrebox_daemon_mailbox_list_result_get_entry (&result, 0);
  g_assert_cmpstr (entry->mailbox_id, ==, "mailbox-1");
  g_assert_cmpstr (entry->mailbox_name, ==, "Projects");
  g_assert_cmpstr (entry->hierarchy_delimiter, ==, "/");
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
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-project-a", "Projects/Project A", "/", NULL, TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN, &error));
  g_assert_no_error (error);

  entry = wyrebox_daemon_mailbox_list_result_get_entry (&result, 0);
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (entry->hierarchy_delimiter, ==, "/");
  g_assert_cmpstr (entry->special_use, ==, NULL);
  g_assert_true (entry->is_selectable);
  g_assert_cmpint (entry->child_state, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN);
}

static void
test_mailbox_list_result_rejects_invalid_entry_fields (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_false (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "", "INBOX", "/", NULL, TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries (&result),
      ==, 0);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX\n", "/", NULL, TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries (&result),
      ==, 0);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", "\n", NULL, TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries (&result),
      ==, 0);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", "//", NULL, TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries (&result),
      ==, 0);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          (WyreboxDaemonMailboxListEntryKind) 99,
          "mailbox-inbox", "INBOX", "/", NULL, TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries (&result),
      ==, 0);

  g_clear_error (&error);
  g_assert_false (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-inbox", "INBOX", "/", NULL, TRUE,
          (WyreboxDaemonMailboxListChildState) 99, &error));
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
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-1", "Archive", "/", "\\Archive", TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_list_entry_init (&entry,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-1", "Smart/Invoices", "/", NULL, FALSE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN, &error));
  g_assert_no_error (error);

  g_assert_cmpint (entry.kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (entry.mailbox_id, ==, "view-1");
  g_assert_cmpstr (entry.mailbox_name, ==, "Smart/Invoices");
  g_assert_cmpstr (entry.hierarchy_delimiter, ==, "/");
  g_assert_cmpstr (entry.special_use, ==, NULL);
  g_assert_false (entry.is_selectable);
  g_assert_cmpint (entry.child_state, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN);
}

static void
test_mailbox_list_entry_failure_preserves_existing_contents (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxListEntry) entry = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_list_entry_init (&entry,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
          "mailbox-1", "Archive", "/", "\\Archive", TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_mailbox_list_entry_init (&entry,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-1", "Smart/Invoices", "//", NULL, FALSE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_assert_cmpint (entry.kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  g_assert_cmpstr (entry.mailbox_id, ==, "mailbox-1");
  g_assert_cmpstr (entry.mailbox_name, ==, "Archive");
  g_assert_cmpstr (entry.hierarchy_delimiter, ==, "/");
  g_assert_cmpstr (entry.special_use, ==, "\\Archive");
  g_assert_true (entry.is_selectable);
  g_assert_cmpint (entry.child_state, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN);
}

static void
test_mailbox_list_result_projection_without_deliveries_is_empty (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDeliveryProjectionList) projection = { 0 };
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };

  projection.records = g_ptr_array_new ();

  g_assert_true
      (wyrebox_daemon_mailbox_list_result_init_from_delivery_projection
      (&result, &projection, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries (&result),
      ==, 0);
}

static void
test_mailbox_list_result_projection_with_deliveries_exposes_inbox (void)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDeliveryProjectionList) projection = { 0 };
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  const WyreboxDaemonMailboxListEntry *entry = NULL;

  projection.records = g_ptr_array_new ();
  g_ptr_array_add (projection.records, GINT_TO_POINTER (1));

  g_assert_true
      (wyrebox_daemon_mailbox_list_result_init_from_delivery_projection
      (&result, &projection, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries (&result),
      ==, 1);

  entry = wyrebox_daemon_mailbox_list_result_get_entry (&result, 0);
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  g_assert_cmpstr (entry->mailbox_id, ==, "mailbox-inbox");
  g_assert_cmpstr (entry->mailbox_name, ==, "INBOX");
  g_assert_cmpstr (entry->hierarchy_delimiter, ==, "/");
  g_assert_cmpstr (entry->special_use, ==, "\\Inbox");
  g_assert_true (entry->is_selectable);
  g_assert_cmpint (entry->child_state, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN);
}

static void
test_mailbox_list_result_projection_failure_preserves_existing_contents (void)
{
  g_autoptr (GError) error = NULL;
  WyreboxDeliveryProjectionList invalid = { 0 };
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  const WyreboxDaemonMailboxListEntry *entry = NULL;

  wyrebox_daemon_mailbox_list_result_init_empty (&result);
  g_assert_true (wyrebox_daemon_mailbox_list_result_append_entry (&result,
          WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
          "view-1", "Smart/Invoices", "/", NULL, TRUE,
          WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN, &error));
  g_assert_no_error (error);

  g_assert_false
      (wyrebox_daemon_mailbox_list_result_init_from_delivery_projection
      (&result, &invalid, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries (&result),
      ==, 1);
  entry = wyrebox_daemon_mailbox_list_result_get_entry (&result, 0);
  g_assert_cmpint (entry->kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (entry->mailbox_id, ==, "view-1");
  g_assert_cmpstr (entry->mailbox_name, ==, "Smart/Invoices");
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
  g_test_add_func ("/daemon-api/mailbox-list-result/"
      "entry-failure-preserves-existing-contents",
      test_mailbox_list_entry_failure_preserves_existing_contents);
  g_test_add_func ("/daemon-api/mailbox-list-result/"
      "projection-without-deliveries-is-empty",
      test_mailbox_list_result_projection_without_deliveries_is_empty);
  g_test_add_func ("/daemon-api/mailbox-list-result/"
      "projection-with-deliveries-exposes-inbox",
      test_mailbox_list_result_projection_with_deliveries_exposes_inbox);
  g_test_add_func ("/daemon-api/mailbox-list-result/"
      "projection-failure-preserves-existing-contents",
      test_mailbox_list_result_projection_failure_preserves_existing_contents);

  return g_test_run ();
}
