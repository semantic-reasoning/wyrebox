#include "wyrebox-daemon-mailbox-catalog-duckdb.h"
#include "wyrebox-schema-metadata-store.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

static void
duckdb_connection_clear (duckdb_connection *connection)
{
  duckdb_disconnect (connection);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_connection, duckdb_connection_clear)
/* *INDENT-ON* */

static void
duckdb_database_clear (duckdb_database *database)
{
  duckdb_close (database);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_database, duckdb_database_clear)
/* *INDENT-ON* */

static void
exec_sql (duckdb_connection connection, const gchar *sql)
{
  duckdb_result result = { 0 };
  duckdb_state state = duckdb_query (connection, sql, &result);

  if (state != DuckDBSuccess)
    g_error ("duckdb query failed: %s", duckdb_result_error (&result));

  duckdb_destroy_result (&result);
}

static gchar *
create_temp_catalog_path (void)
{
  gint fd = -1;
  gchar *path = NULL;

  fd = g_file_open_tmp ("wyrebox-mailbox-catalog-XXXXXX.duckdb", &path, NULL);
  g_assert_cmpint (fd, >=, 0);
  g_assert_nonnull (path);

  g_assert_true (g_close (fd, NULL));
  (void) g_remove (path);
  return path;
}

static void
bootstrap_catalog (const gchar *path)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, 1, &error));
  g_assert_no_error (error);
}

static void
seed_catalog (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO accounts (account_id) VALUES ('account-1'), "
      "('account-2');");
  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('msg-1', 'account-1', 'obj-1', 1, 1),"
      "('msg-2', 'account-1', 'obj-2', 2, 2),"
      "('msg-hidden', 'account-1', 'obj-hidden', 3, 3),"
      "('msg-view', 'account-1', 'obj-view', 4, 4),"
      "('msg-other', 'account-2', 'obj-other', 5, 5),"
      "('msg-sql', 'account-1', 'obj-sql', 6, 6),"
      "('msg-view-sql', 'account-1', 'obj-view-sql', 7, 7);");
  exec_sql (connection,
      "INSERT INTO mailboxes (mailbox_id, account_id, imap_name, "
      "is_selectable, is_visible) VALUES "
      "('mb-inbox', 'account-1', 'INBOX', TRUE, TRUE),"
      "('mb-projects', 'account-1', 'Projects', FALSE, TRUE),"
      "('mb-project-alpha', 'account-1', 'Projects/Alpha', TRUE, TRUE),"
      "('mb-projectsx', 'account-1', 'ProjectsX', TRUE, TRUE),"
      "('mb-hidden', 'account-1', 'Hidden', TRUE, FALSE),"
      "('mb-other', 'account-2', 'Other Account', TRUE, TRUE),"
      "('mb-conflict', 'account-1', 'Shared', TRUE, TRUE),"
      "('mb''; DROP TABLE mailboxes; --', 'account-1', "
      "'Name''; DROP TABLE derived_views; --', TRUE, TRUE);");
  exec_sql (connection,
      "INSERT INTO derived_views (view_id, account_id, imap_name, "
      "definition_ref, is_selectable, is_visible) VALUES "
      "('view-important', 'account-1', 'Important', 'rule:important', TRUE, "
      "TRUE),"
      "('view-containers', 'account-1', 'Views', 'rule:containers', FALSE, "
      "TRUE),"
      "('view-hidden', 'account-1', 'Hidden View', 'rule:hidden', TRUE, "
      "FALSE),"
      "('view-other', 'account-2', 'Other View', 'rule:other', TRUE, TRUE),"
      "('view-conflict', 'account-1', 'Shared', 'rule:shared', TRUE, TRUE),"
      "('view''; DROP TABLE mailboxes; --', 'account-1', "
      "'View''; DROP TABLE derived_views; --', 'rule:sql', TRUE, TRUE);");
  exec_sql (connection,
      "INSERT INTO mailbox_uid_state (account_id, namespace_kind, "
      "namespace_id, uidnext, uidvalidity) VALUES "
      "('account-1', 'mailbox', 'mb-inbox', 4, 11),"
      "('account-1', 'mailbox', 'mb-projects', 1, 16),"
      "('account-1', 'mailbox', 'mb-project-alpha', 2, 17),"
      "('account-1', 'mailbox', 'mb-projectsx', 2, 18),"
      "('account-1', 'mailbox', 'mb-hidden', 2, 12),"
      "('account-2', 'mailbox', 'mb-other', 2, 13),"
      "('account-1', 'mailbox', 'mb-conflict', 2, 14),"
      "('account-1', 'mailbox', 'mb''; DROP TABLE mailboxes; --', 2, 15),"
      "('account-1', 'derived_view', 'view-important', 3, 21),"
      "('account-1', 'derived_view', 'view-containers', 1, 26),"
      "('account-1', 'derived_view', 'view-hidden', 2, 22),"
      "('account-2', 'derived_view', 'view-other', 2, 23),"
      "('account-1', 'derived_view', 'view-conflict', 2, 24),"
      "('account-1', 'derived_view', "
      "'view''; DROP TABLE mailboxes; --', 2, 25);");
  exec_sql (connection,
      "INSERT INTO mailbox_memberships (membership_id, account_id, "
      "mailbox_id, message_id, uid, internal_date_unix_us, journal_offset, "
      "journal_sequence, is_visible) VALUES "
      "('mm-1', 'account-1', 'mb-inbox', 'msg-1', 1, 1, 1, 1, TRUE),"
      "('mm-2', 'account-1', 'mb-inbox', 'msg-2', 2, 2, 2, 2, TRUE),"
      "('mm-hidden', 'account-1', 'mb-inbox', 'msg-hidden', 3, 3, 3, 3, "
      "FALSE),"
      "('mm-project-alpha', 'account-1', 'mb-project-alpha', 'msg-view', 1, "
      "4, 4, 4, TRUE),"
      "('mm-projectsx', 'account-1', 'mb-projectsx', 'msg-view', 1, "
      "4, 4, 4, TRUE),"
      "('mm-other', 'account-2', 'mb-other', 'msg-other', 1, 4, 4, 4, TRUE),"
      "('mm-sql', 'account-1', 'mb''; DROP TABLE mailboxes; --', "
      "'msg-sql', 1, 5, 5, 5, TRUE);");
  exec_sql (connection,
      "INSERT INTO derived_view_memberships (membership_id, account_id, "
      "view_id, message_id, uid, is_visible, rule_version_hash, "
      "materialized_at_unix_us) VALUES "
      "('dvm-1', 'account-1', 'view-important', 'msg-1', 1, TRUE, "
      "'hash-1', 1),"
      "('dvm-hidden', 'account-1', 'view-important', 'msg-hidden', 2, FALSE, "
      "'hash-1', 2),"
      "('dvm-other', 'account-2', 'view-other', 'msg-other', 1, TRUE, "
      "'hash-2', 3),"
      "('dvm-sql', 'account-1', 'view''; DROP TABLE mailboxes; --', "
      "'msg-view-sql', 1, TRUE, 'hash-3', 4);");
}

static const WyreboxDaemonMailboxListEntry *
find_entry (const WyreboxDaemonMailboxListResult *result, const gchar *name)
{
  for (guint i = 0;
      i < wyrebox_daemon_mailbox_list_result_get_n_entries (result); i++) {
    const WyreboxDaemonMailboxListEntry *entry =
        wyrebox_daemon_mailbox_list_result_get_entry (result, i);

    if (g_strcmp0 (entry->mailbox_name, name) == 0)
      return entry;
  }

  return NULL;
}

static void
assert_select (WyreboxDaemonMailboxCatalogDuckDB *catalog,
    const gchar *mailbox_id,
    const gchar *mailbox_name,
    WyreboxDaemonMailboxListEntryKind expected_kind,
    const gchar *expected_id,
    const gchar *expected_name,
    guint32 expected_uid_validity,
    guint32 expected_uid_next, guint32 expected_message_count)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", mailbox_id, mailbox_name, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_daemon_mailbox_catalog_duckdb_select (NULL, &request,
          &result, catalog, &error));
  g_assert_no_error (error);
  g_assert_cmpint (result.kind, ==, expected_kind);
  g_assert_cmpstr (result.mailbox_id, ==, expected_id);
  g_assert_cmpstr (result.mailbox_name, ==, expected_name);
  g_assert_cmpuint (result.uid_validity, ==, expected_uid_validity);
  g_assert_cmpuint (result.uid_next, ==, expected_uid_next);
  g_assert_cmpuint (result.message_count, ==, expected_message_count);
}

static void
assert_select_not_found (WyreboxDaemonMailboxCatalogDuckDB *catalog,
    const gchar *account_id, const gchar *mailbox_id, const gchar *mailbox_name)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          account_id, mailbox_id, mailbox_name, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_mailbox_catalog_duckdb_select (NULL, &request,
          &result, catalog, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

static void
assert_select_conflict (WyreboxDaemonMailboxCatalogDuckDB *catalog,
    const gchar *account_id, const gchar *mailbox_id, const gchar *mailbox_name)
{
  g_autoptr (GError) error = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          account_id, mailbox_id, mailbox_name, &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_mailbox_catalog_duckdb_select (NULL, &request,
          &result, catalog, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);
}

static void
test_list_and_select_from_duckdb_catalog (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree gchar *path = NULL;
  g_autoptr (WyreboxDaemonMailboxCatalogDuckDB) catalog = NULL;
  g_auto (WyreboxDaemonMailboxListRequest) list_request = { 0 };
  g_auto (WyreboxDaemonMailboxListResult) list_result = { 0 };
  g_auto (WyreboxDaemonMailboxListRequest) prefixed_list_request = { 0 };
  g_auto (WyreboxDaemonMailboxListResult) prefixed_list_result = { 0 };
  const WyreboxDaemonMailboxListEntry *entry = NULL;

  path = create_temp_catalog_path ();
  bootstrap_catalog (path);
  seed_catalog (path);

  catalog = wyrebox_daemon_mailbox_catalog_duckdb_new (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (catalog);

  g_assert_true (wyrebox_daemon_mailbox_list_request_init (&list_request,
          "account-1", "", &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_catalog_duckdb_list (NULL,
          &list_request, &list_result, catalog, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries
      (&list_result), ==, 10);

  entry = find_entry (&list_result, "INBOX");
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  g_assert_cmpstr (entry->mailbox_id, ==, "mb-inbox");
  g_assert_cmpstr (entry->hierarchy_delimiter, ==, "/");
  g_assert_null (entry->special_use);
  g_assert_true (entry->is_selectable);
  g_assert_cmpint (entry->child_state, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN);

  entry = find_entry (&list_result, "Projects");
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  g_assert_cmpstr (entry->mailbox_id, ==, "mb-projects");
  g_assert_false (entry->is_selectable);
  g_assert_cmpint (entry->child_state, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN);

  entry = find_entry (&list_result, "Projects/Alpha");
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY);
  g_assert_cmpstr (entry->mailbox_id, ==, "mb-project-alpha");
  g_assert_true (entry->is_selectable);
  g_assert_cmpint (entry->child_state, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN);

  entry = find_entry (&list_result, "ProjectsX");
  g_assert_nonnull (entry);
  g_assert_cmpstr (entry->mailbox_id, ==, "mb-projectsx");
  g_assert_cmpint (entry->child_state, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN);

  entry = find_entry (&list_result, "Important");
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (entry->mailbox_id, ==, "view-important");
  g_assert_true (entry->is_selectable);

  entry = find_entry (&list_result, "Views");
  g_assert_nonnull (entry);
  g_assert_cmpint (entry->kind, ==, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL);
  g_assert_cmpstr (entry->mailbox_id, ==, "view-containers");
  g_assert_false (entry->is_selectable);
  g_assert_cmpint (entry->child_state, ==,
      WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN);

  g_assert_null (find_entry (&list_result, "Hidden"));
  g_assert_null (find_entry (&list_result, "Hidden View"));
  g_assert_null (find_entry (&list_result, "Other Account"));
  g_assert_null (find_entry (&list_result, "Other View"));

  g_assert_true (wyrebox_daemon_mailbox_list_request_init
      (&prefixed_list_request, "account-1", "Projects", &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_daemon_mailbox_catalog_duckdb_list (NULL,
          &prefixed_list_request, &prefixed_list_result, catalog, &error));
  g_assert_no_error (error);
  g_assert_cmpuint (wyrebox_daemon_mailbox_list_result_get_n_entries
      (&prefixed_list_result), ==, 2);
  g_assert_nonnull (find_entry (&prefixed_list_result, "Projects"));
  g_assert_nonnull (find_entry (&prefixed_list_result, "Projects/Alpha"));
  g_assert_null (find_entry (&prefixed_list_result, "ProjectsX"));
  g_assert_null (find_entry (&prefixed_list_result, "INBOX"));
  g_assert_null (find_entry (&prefixed_list_result, "Important"));

  assert_select (catalog, NULL, "INBOX",
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, "mb-inbox", "INBOX", 11, 4,
      2);
  assert_select (catalog, "mb-inbox", NULL,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, "mb-inbox", "INBOX", 11, 4,
      2);
  assert_select (catalog, NULL, "Important",
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL, "view-important",
      "Important", 21, 3, 1);
  assert_select (catalog, "view-important", NULL,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL, "view-important",
      "Important", 21, 3, 1);
  assert_select (catalog, "mb'; DROP TABLE mailboxes; --", NULL,
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
      "mb'; DROP TABLE mailboxes; --", "Name'; DROP TABLE derived_views; --",
      15, 2, 1);
  assert_select (catalog, NULL, "View'; DROP TABLE derived_views; --",
      WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
      "view'; DROP TABLE mailboxes; --",
      "View'; DROP TABLE derived_views; --", 25, 2, 1);

  assert_select_not_found (catalog, "account-1", "mb-hidden", NULL);
  assert_select_not_found (catalog, "account-1", NULL, "Hidden");
  assert_select_conflict (catalog, "account-1", "mb-projects", NULL);
  assert_select_conflict (catalog, "account-1", NULL, "Projects");
  assert_select_conflict (catalog, "account-1", "view-containers", NULL);
  assert_select_conflict (catalog, "account-1", NULL, "Views");
  assert_select_not_found (catalog, "account-1", "view-hidden", NULL);
  assert_select_not_found (catalog, "account-1", NULL, "Hidden View");
  assert_select_not_found (catalog, "account-1", "mb-other", NULL);
  assert_select_not_found (catalog, "account-1", NULL, "Other Account");

  (void) g_remove (path);
}

static void
test_select_by_name_conflict_fails (void)
{
  g_autoptr (GError) error = NULL;
  g_autofree gchar *path = NULL;
  g_autoptr (WyreboxDaemonMailboxCatalogDuckDB) catalog = NULL;
  g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
  g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };

  path = create_temp_catalog_path ();
  bootstrap_catalog (path);
  seed_catalog (path);

  catalog = wyrebox_daemon_mailbox_catalog_duckdb_new (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (catalog);

  g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
          "account-1", NULL, "Shared", &error));
  g_assert_no_error (error);

  g_assert_false (wyrebox_daemon_mailbox_catalog_duckdb_select (NULL, &request,
          &result, catalog, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_EXISTS);

  (void) g_remove (path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/wyrebox/daemon/mailbox-catalog-duckdb/list-select",
      test_list_and_select_from_duckdb_catalog);
  g_test_add_func ("/wyrebox/daemon/mailbox-catalog-duckdb/conflict",
      test_select_by_name_conflict_fails);

  return g_test_run ();
}
