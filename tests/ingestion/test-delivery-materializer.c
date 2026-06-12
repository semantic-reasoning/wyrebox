#include "wyrebox-delivery-materializer.h"
#include "wyrebox-delivery-projection.h"
#include "wyrebox-schema-metadata-store.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

typedef char *TestDuckdbOwnedString;

typedef struct
{
  duckdb_database database;
  duckdb_connection connection;
} TestDuckdbFixture;

static void
remove_tree (const char *path)
{
  g_autoptr (GDir) dir = NULL;
  const char *name = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_remove (path);
    return;
  }

  while ((name = g_dir_read_name (dir)) != NULL) {
    g_autofree char *child = g_build_filename (path, name, NULL);

    remove_tree (child);
  }

  (void) g_rmdir (path);
}

static void
duckdb_result_clear (duckdb_result *result)
{
  duckdb_destroy_result (result);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_result, duckdb_result_clear)
/* *INDENT-ON* */

static void
duckdb_owned_string_clear (char **value)
{
  if (value != NULL && *value != NULL)
    duckdb_free (*value);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (TestDuckdbOwnedString,
    duckdb_owned_string_clear)
/* *INDENT-ON* */

static void
projection_record_free (gpointer data)
{
  WyreboxDeliveryProjectionRecord *record = data;

  if (record == NULL)
    return;

  wyrebox_delivery_projection_record_clear (record);
  g_free (record);
}

static void
append_projection_record (WyreboxDeliveryProjectionList *projection,
    const gchar *object_key, guint64 size_bytes, guint64 internal_date_unix_us,
    guint64 journal_offset, guint64 journal_sequence)
{
  WyreboxDeliveryProjectionRecord *record = NULL;

  if (projection->records == NULL)
    projection->records =
        g_ptr_array_new_with_free_func (projection_record_free);

  record = g_new0 (WyreboxDeliveryProjectionRecord, 1);
  record->object_key = g_strdup (object_key);
  record->size_bytes = size_bytes;
  record->internal_date_unix_us = internal_date_unix_us;
  record->journal_offset = journal_offset;
  record->journal_sequence = journal_sequence;

  g_ptr_array_add (projection->records, record);
}

static gchar *
create_bootstrap_catalog (void)
{
  g_autofree gchar *dir = NULL;
  g_autofree gchar *path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

  dir = g_dir_make_tmp ("wyrebox-delivery-materializer-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  path = g_build_filename (dir, "catalog.duckdb", NULL);
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, 1, &error));
  g_assert_no_error (error);

  return g_steal_pointer (&path);
}

static void
remove_catalog (const gchar *path)
{
  g_autofree gchar *dir = g_path_get_dirname (path);

  remove_tree (dir);
}

static void
open_duckdb_fixture (const gchar *path, TestDuckdbFixture *fixture)
{
  g_assert_cmpint (duckdb_open (path, &fixture->database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (fixture->database, &fixture->connection),
      ==, DuckDBSuccess);
}

static void
close_duckdb_fixture (TestDuckdbFixture *fixture)
{
  if (fixture->connection != NULL)
    duckdb_disconnect (&fixture->connection);
  if (fixture->database != NULL)
    duckdb_close (&fixture->database);
}

static guint64
query_uint64 (duckdb_connection connection, const gchar *sql)
{
  g_auto (duckdb_result) result = { 0 };

  g_assert_cmpint (duckdb_query (connection, sql, &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
  g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
  g_assert_false (duckdb_value_is_null (&result, 0, 0));

  return (guint64) duckdb_value_uint64 (&result, 0, 0);
}

static gchar *
query_string (duckdb_connection connection, const gchar *sql)
{
  g_auto (duckdb_result) result = { 0 };
  g_auto (TestDuckdbOwnedString) value = NULL;

  g_assert_cmpint (duckdb_query (connection, sql, &result), ==, DuckDBSuccess);
  g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
  g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
  g_assert_false (duckdb_value_is_null (&result, 0, 0));

  value = duckdb_value_varchar (&result, 0, 0);
  return g_strdup (value);
}

static void
execute_sql (duckdb_connection connection, const gchar *sql)
{
  g_auto (duckdb_result) result = { 0 };

  g_assert_cmpint (duckdb_query (connection, sql, &result), ==, DuckDBSuccess);
}

static void
assert_table_count (duckdb_connection connection, const gchar *table,
    guint64 expected)
{
  g_autofree gchar *sql = g_strdup_printf ("SELECT COUNT(*) FROM %s;", table);

  g_assert_cmpuint (query_uint64 (connection, sql), ==, expected);
}

static void
assert_materialization_checkpoint (duckdb_connection connection,
    guint64 expected_offset, guint64 expected_sequence)
{
  g_assert_cmpuint (query_uint64 (connection,
          "SELECT COUNT(*) FROM materialization_checkpoint WHERE "
          "checkpoint_key = 'materialization';"), ==, 1);
  g_assert_cmpuint (query_uint64 (connection,
          "SELECT journal_offset FROM materialization_checkpoint WHERE "
          "checkpoint_key = 'materialization';"), ==, expected_offset);
  g_assert_cmpuint (query_uint64 (connection,
          "SELECT journal_sequence FROM materialization_checkpoint WHERE "
          "checkpoint_key = 'materialization';"), ==, expected_sequence);
}

static void
assert_apply_to_inbox_fails_invalid_data (const gchar *path,
    const WyreboxDeliveryProjectionList *projection)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDeliveryMaterializer) materializer = NULL;

  materializer = wyrebox_delivery_materializer_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);
  g_assert_false (wyrebox_delivery_materializer_apply_to_mailbox (materializer,
          "account-1", "mailbox-inbox", "INBOX", projection, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
apply_projection_to_inbox (const gchar *path,
    const WyreboxDeliveryProjectionList *projection)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDeliveryMaterializer) materializer = NULL;

  materializer = wyrebox_delivery_materializer_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);
  g_assert_true (wyrebox_delivery_materializer_apply_to_mailbox (materializer,
          "account-1", "mailbox-inbox", "INBOX", projection, &error));
  g_assert_no_error (error);
}

static void
test_divergent_mailbox_conflict_rolls_back (void)
{
  g_autofree gchar *path = create_bootstrap_catalog ();
  g_auto (WyreboxDeliveryProjectionList) projection = { 0 };
  TestDuckdbFixture duckdb = { 0 };

  append_projection_record (&projection, "sha256:first", 101, 1001, 11, 1);

  open_duckdb_fixture (path, &duckdb);
  execute_sql (duckdb.connection,
      "INSERT INTO mailboxes ("
      "mailbox_id, account_id, imap_name, is_selectable, is_visible"
      ") VALUES ('mailbox-other', 'account-1', 'INBOX', TRUE, TRUE);");
  close_duckdb_fixture (&duckdb);

  assert_apply_to_inbox_fails_invalid_data (path, &projection);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "accounts", 0);
  assert_table_count (duckdb.connection, "mailboxes", 1);
  assert_table_count (duckdb.connection, "mailbox_uid_state", 0);
  assert_table_count (duckdb.connection, "objects", 0);
  assert_table_count (duckdb.connection, "messages", 0);
  assert_table_count (duckdb.connection, "mailbox_memberships", 0);
  g_assert_cmpstr (query_string (duckdb.connection,
          "SELECT mailbox_id FROM mailboxes WHERE account_id = 'account-1' "
          "AND imap_name = 'INBOX';"), ==, "mailbox-other");
  close_duckdb_fixture (&duckdb);

  remove_catalog (path);
}

static void
test_divergent_object_conflict_rolls_back (void)
{
  g_autofree gchar *path = create_bootstrap_catalog ();
  g_auto (WyreboxDeliveryProjectionList) projection = { 0 };
  TestDuckdbFixture duckdb = { 0 };

  append_projection_record (&projection, "sha256:first", 101, 1001, 11, 1);

  open_duckdb_fixture (path, &duckdb);
  execute_sql (duckdb.connection,
      "INSERT INTO objects (object_id, size_bytes) "
      "VALUES ('sha256:first', 999);");
  close_duckdb_fixture (&duckdb);

  assert_apply_to_inbox_fails_invalid_data (path, &projection);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "accounts", 0);
  assert_table_count (duckdb.connection, "mailboxes", 0);
  assert_table_count (duckdb.connection, "mailbox_uid_state", 0);
  assert_table_count (duckdb.connection, "objects", 1);
  assert_table_count (duckdb.connection, "messages", 0);
  assert_table_count (duckdb.connection, "mailbox_memberships", 0);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT size_bytes FROM objects WHERE object_id = 'sha256:first';"),
      ==, 999);
  close_duckdb_fixture (&duckdb);

  remove_catalog (path);
}

static void
test_divergent_message_conflict_rolls_back (void)
{
  g_autofree gchar *path = create_bootstrap_catalog ();
  g_auto (WyreboxDeliveryProjectionList) projection = { 0 };
  TestDuckdbFixture duckdb = { 0 };

  append_projection_record (&projection, "sha256:first", 101, 1001, 11, 1);

  open_duckdb_fixture (path, &duckdb);
  execute_sql (duckdb.connection,
      "INSERT INTO objects (object_id, size_bytes) "
      "VALUES ('sha256:wrong', 999);");
  execute_sql (duckdb.connection,
      "INSERT INTO messages ("
      "message_id, account_id, object_id, journal_offset, journal_sequence"
      ") VALUES ('journal:11:1', 'account-other', 'sha256:wrong', 11, 1);");
  close_duckdb_fixture (&duckdb);

  assert_apply_to_inbox_fails_invalid_data (path, &projection);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "accounts", 0);
  assert_table_count (duckdb.connection, "mailboxes", 0);
  assert_table_count (duckdb.connection, "mailbox_uid_state", 0);
  assert_table_count (duckdb.connection, "objects", 1);
  assert_table_count (duckdb.connection, "messages", 1);
  assert_table_count (duckdb.connection, "mailbox_memberships", 0);
  g_assert_cmpstr (query_string (duckdb.connection,
          "SELECT object_id FROM messages WHERE message_id = 'journal:11:1';"),
      ==, "sha256:wrong");
  close_duckdb_fixture (&duckdb);

  remove_catalog (path);
}

static void
test_divergent_membership_conflict_rolls_back (void)
{
  g_autofree gchar *path = create_bootstrap_catalog ();
  g_auto (WyreboxDeliveryProjectionList) projection = { 0 };
  TestDuckdbFixture duckdb = { 0 };

  append_projection_record (&projection, "sha256:first", 101, 1001, 11, 1);

  open_duckdb_fixture (path, &duckdb);
  execute_sql (duckdb.connection,
      "INSERT INTO objects (object_id, size_bytes) "
      "VALUES ('sha256:first', 101);");
  execute_sql (duckdb.connection,
      "INSERT INTO messages ("
      "message_id, account_id, object_id, journal_offset, journal_sequence"
      ") VALUES ('journal:11:1', 'account-1', 'sha256:first', 11, 1);");
  execute_sql (duckdb.connection,
      "INSERT INTO mailbox_memberships ("
      "membership_id, account_id, mailbox_id, message_id, uid, "
      "internal_date_unix_us, journal_offset, journal_sequence, is_visible"
      ") VALUES ("
      "'mailbox:mailbox-inbox:journal:11:1', "
      "'account-1', 'mailbox-inbox', 'journal:11:1', 7, "
      "9999, 11, 1, TRUE);");
  close_duckdb_fixture (&duckdb);

  assert_apply_to_inbox_fails_invalid_data (path, &projection);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "accounts", 0);
  assert_table_count (duckdb.connection, "mailboxes", 0);
  assert_table_count (duckdb.connection, "mailbox_uid_state", 0);
  assert_table_count (duckdb.connection, "objects", 1);
  assert_table_count (duckdb.connection, "messages", 1);
  assert_table_count (duckdb.connection, "mailbox_memberships", 1);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT internal_date_unix_us FROM mailbox_memberships WHERE "
          "membership_id = 'mailbox:mailbox-inbox:journal:11:1';"), ==, 9999);
  close_duckdb_fixture (&duckdb);

  remove_catalog (path);
}

static void
test_existing_membership_uid_is_preserved (void)
{
  g_autofree gchar *path = create_bootstrap_catalog ();
  g_auto (WyreboxDeliveryProjectionList) projection = { 0 };
  TestDuckdbFixture duckdb = { 0 };

  append_projection_record (&projection, "sha256:first", 101, 1001, 11, 1);

  open_duckdb_fixture (path, &duckdb);
  execute_sql (duckdb.connection,
      "INSERT INTO objects (object_id, size_bytes) "
      "VALUES ('sha256:first', 101);");
  execute_sql (duckdb.connection,
      "INSERT INTO messages ("
      "message_id, account_id, object_id, journal_offset, journal_sequence"
      ") VALUES ('journal:11:1', 'account-1', 'sha256:first', 11, 1);");
  execute_sql (duckdb.connection,
      "INSERT INTO mailbox_memberships ("
      "membership_id, account_id, mailbox_id, message_id, uid, "
      "internal_date_unix_us, journal_offset, journal_sequence, is_visible"
      ") VALUES ("
      "'mailbox:mailbox-inbox:journal:11:1', "
      "'account-1', 'mailbox-inbox', 'journal:11:1', 2, "
      "1001, 11, 1, TRUE);");
  execute_sql (duckdb.connection,
      "INSERT INTO mailbox_uid_state ("
      "account_id, namespace_kind, namespace_id, uidnext, uidvalidity"
      ") VALUES ('account-1', 'mailbox', 'mailbox-inbox', 3, 1);");
  close_duckdb_fixture (&duckdb);

  apply_projection_to_inbox (path, &projection);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "accounts", 1);
  assert_table_count (duckdb.connection, "mailboxes", 1);
  assert_table_count (duckdb.connection, "mailbox_uid_state", 1);
  assert_table_count (duckdb.connection, "objects", 1);
  assert_table_count (duckdb.connection, "messages", 1);
  assert_table_count (duckdb.connection, "mailbox_memberships", 1);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uid FROM mailbox_memberships WHERE "
          "membership_id = 'mailbox:mailbox-inbox:journal:11:1';"), ==, 2);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uidnext FROM mailbox_uid_state WHERE "
          "account_id = 'account-1' AND namespace_kind = 'mailbox' "
          "AND namespace_id = 'mailbox-inbox';"), ==, 3);
  close_duckdb_fixture (&duckdb);

  remove_catalog (path);
}

static void
test_wrong_uidvalidity_rolls_back (void)
{
  g_autofree gchar *path = create_bootstrap_catalog ();
  g_auto (WyreboxDeliveryProjectionList) projection = { 0 };
  TestDuckdbFixture duckdb = { 0 };

  append_projection_record (&projection, "sha256:first", 101, 1001, 11, 1);

  open_duckdb_fixture (path, &duckdb);
  execute_sql (duckdb.connection,
      "INSERT INTO mailbox_uid_state ("
      "account_id, namespace_kind, namespace_id, uidnext, uidvalidity"
      ") VALUES ('account-1', 'mailbox', 'mailbox-inbox', 1, 2);");
  close_duckdb_fixture (&duckdb);

  assert_apply_to_inbox_fails_invalid_data (path, &projection);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "accounts", 0);
  assert_table_count (duckdb.connection, "mailboxes", 0);
  assert_table_count (duckdb.connection, "mailbox_uid_state", 1);
  assert_table_count (duckdb.connection, "objects", 0);
  assert_table_count (duckdb.connection, "messages", 0);
  assert_table_count (duckdb.connection, "mailbox_memberships", 0);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uidvalidity FROM mailbox_uid_state WHERE "
          "account_id = 'account-1' AND namespace_kind = 'mailbox' "
          "AND namespace_id = 'mailbox-inbox';"), ==, 2);
  close_duckdb_fixture (&duckdb);

  remove_catalog (path);
}

static void
test_stale_uidnext_rolls_back (void)
{
  g_autofree gchar *path = create_bootstrap_catalog ();
  g_auto (WyreboxDeliveryProjectionList) projection = { 0 };
  TestDuckdbFixture duckdb = { 0 };

  append_projection_record (&projection, "sha256:second", 202, 1002, 22, 2);

  open_duckdb_fixture (path, &duckdb);
  execute_sql (duckdb.connection,
      "INSERT INTO objects (object_id, size_bytes) "
      "VALUES ('sha256:first', 101);");
  execute_sql (duckdb.connection,
      "INSERT INTO messages ("
      "message_id, account_id, object_id, journal_offset, journal_sequence"
      ") VALUES ('journal:11:1', 'account-1', 'sha256:first', 11, 1);");
  execute_sql (duckdb.connection,
      "INSERT INTO mailbox_memberships ("
      "membership_id, account_id, mailbox_id, message_id, uid, "
      "internal_date_unix_us, journal_offset, journal_sequence, is_visible"
      ") VALUES ("
      "'mailbox:mailbox-inbox:journal:11:1', "
      "'account-1', 'mailbox-inbox', 'journal:11:1', 2, "
      "1001, 11, 1, TRUE);");
  execute_sql (duckdb.connection,
      "INSERT INTO mailbox_uid_state ("
      "account_id, namespace_kind, namespace_id, uidnext, uidvalidity"
      ") VALUES ('account-1', 'mailbox', 'mailbox-inbox', 2, 1);");
  close_duckdb_fixture (&duckdb);

  assert_apply_to_inbox_fails_invalid_data (path, &projection);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "accounts", 0);
  assert_table_count (duckdb.connection, "mailboxes", 0);
  assert_table_count (duckdb.connection, "mailbox_uid_state", 1);
  assert_table_count (duckdb.connection, "objects", 1);
  assert_table_count (duckdb.connection, "messages", 1);
  assert_table_count (duckdb.connection, "mailbox_memberships", 1);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uidnext FROM mailbox_uid_state WHERE "
          "account_id = 'account-1' AND namespace_kind = 'mailbox' "
          "AND namespace_id = 'mailbox-inbox';"), ==, 2);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uid FROM mailbox_memberships WHERE "
          "membership_id = 'mailbox:mailbox-inbox:journal:11:1';"), ==, 2);
  close_duckdb_fixture (&duckdb);

  remove_catalog (path);
}

static void
test_happy_path_materializes_inbox (void)
{
  g_autofree gchar *path = create_bootstrap_catalog ();
  g_auto (WyreboxDeliveryProjectionList) projection = { 0 };
  TestDuckdbFixture duckdb = { 0 };
  g_autofree gchar *first_message_id = NULL;
  g_autofree gchar *first_membership_id = NULL;

  append_projection_record (&projection, "sha256:first", 101, 1001, 11, 1);
  append_projection_record (&projection, "sha256:second", 202, 1002, 22, 2);

  apply_projection_to_inbox (path, &projection);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "accounts", 1);
  assert_table_count (duckdb.connection, "mailboxes", 1);
  assert_table_count (duckdb.connection, "mailbox_uid_state", 1);
  assert_table_count (duckdb.connection, "objects", 2);
  assert_table_count (duckdb.connection, "messages", 2);
  assert_table_count (duckdb.connection, "mailbox_memberships", 2);
  assert_materialization_checkpoint (duckdb.connection, 22, 2);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uidnext FROM mailbox_uid_state WHERE "
          "account_id = 'account-1' AND namespace_kind = 'mailbox' "
          "AND namespace_id = 'mailbox-inbox';"), ==, 3);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uidvalidity FROM mailbox_uid_state WHERE "
          "account_id = 'account-1' AND namespace_kind = 'mailbox' "
          "AND namespace_id = 'mailbox-inbox';"), ==, 1);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uid FROM mailbox_memberships WHERE "
          "membership_id = 'mailbox:mailbox-inbox:journal:11:1';"), ==, 1);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uid FROM mailbox_memberships WHERE "
          "membership_id = 'mailbox:mailbox-inbox:journal:22:2';"), ==, 2);

  first_message_id = query_string (duckdb.connection,
      "SELECT message_id FROM messages WHERE journal_offset = 11 "
      "AND journal_sequence = 1;");
  first_membership_id = query_string (duckdb.connection,
      "SELECT membership_id FROM mailbox_memberships WHERE "
      "journal_offset = 11 AND journal_sequence = 1;");
  g_assert_cmpstr (first_message_id, ==, "journal:11:1");
  g_assert_cmpstr (first_membership_id, ==,
      "mailbox:mailbox-inbox:journal:11:1");
  close_duckdb_fixture (&duckdb);

  remove_catalog (path);
}

static void
test_duplicate_object_materializes_distinct_messages (void)
{
  g_autofree gchar *path = create_bootstrap_catalog ();
  g_auto (WyreboxDeliveryProjectionList) projection = { 0 };
  TestDuckdbFixture duckdb = { 0 };

  append_projection_record (&projection, "sha256:shared", 101, 1001, 11, 1);
  append_projection_record (&projection, "sha256:shared", 101, 1002, 22, 2);

  apply_projection_to_inbox (path, &projection);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "objects", 1);
  assert_table_count (duckdb.connection, "messages", 2);
  assert_table_count (duckdb.connection, "mailbox_memberships", 2);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uid FROM mailbox_memberships WHERE message_id = "
          "'journal:11:1';"), ==, 1);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uid FROM mailbox_memberships WHERE message_id = "
          "'journal:22:2';"), ==, 2);
  close_duckdb_fixture (&duckdb);

  remove_catalog (path);
}

static void
test_reapply_projection_is_idempotent (void)
{
  g_autofree gchar *path = create_bootstrap_catalog ();
  g_auto (WyreboxDeliveryProjectionList) projection = { 0 };
  TestDuckdbFixture duckdb = { 0 };

  append_projection_record (&projection, "sha256:first", 101, 1001, 11, 1);
  append_projection_record (&projection, "sha256:second", 202, 1002, 22, 2);

  apply_projection_to_inbox (path, &projection);
  apply_projection_to_inbox (path, &projection);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "accounts", 1);
  assert_table_count (duckdb.connection, "mailboxes", 1);
  assert_table_count (duckdb.connection, "mailbox_uid_state", 1);
  assert_table_count (duckdb.connection, "objects", 2);
  assert_table_count (duckdb.connection, "messages", 2);
  assert_table_count (duckdb.connection, "mailbox_memberships", 2);
  assert_materialization_checkpoint (duckdb.connection, 22, 2);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uidnext FROM mailbox_uid_state WHERE "
          "account_id = 'account-1' AND namespace_kind = 'mailbox' "
          "AND namespace_id = 'mailbox-inbox';"), ==, 3);
  close_duckdb_fixture (&duckdb);

  remove_catalog (path);
}

static void
test_suffix_overlap_apply_preserves_existing_uids (void)
{
  g_autofree gchar *path = create_bootstrap_catalog ();
  g_auto (WyreboxDeliveryProjectionList) initial = { 0 };
  g_auto (WyreboxDeliveryProjectionList) suffix = { 0 };
  TestDuckdbFixture duckdb = { 0 };

  append_projection_record (&initial, "sha256:first", 101, 1001, 11, 1);
  append_projection_record (&initial, "sha256:second", 202, 1002, 22, 2);
  append_projection_record (&suffix, "sha256:second", 202, 1002, 22, 2);
  append_projection_record (&suffix, "sha256:third", 303, 1003, 33, 3);

  apply_projection_to_inbox (path, &initial);
  apply_projection_to_inbox (path, &suffix);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "objects", 3);
  assert_table_count (duckdb.connection, "messages", 3);
  assert_table_count (duckdb.connection, "mailbox_memberships", 3);
  assert_materialization_checkpoint (duckdb.connection, 33, 3);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uid FROM mailbox_memberships WHERE message_id = "
          "'journal:22:2';"), ==, 2);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uid FROM mailbox_memberships WHERE message_id = "
          "'journal:33:3';"), ==, 3);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uidnext FROM mailbox_uid_state WHERE "
          "account_id = 'account-1' AND namespace_kind = 'mailbox' "
          "AND namespace_id = 'mailbox-inbox';"), ==, 4);
  close_duckdb_fixture (&duckdb);

  apply_projection_to_inbox (path, &suffix);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "objects", 3);
  assert_table_count (duckdb.connection, "messages", 3);
  assert_table_count (duckdb.connection, "mailbox_memberships", 3);
  assert_materialization_checkpoint (duckdb.connection, 33, 3);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uidnext FROM mailbox_uid_state WHERE "
          "account_id = 'account-1' AND namespace_kind = 'mailbox' "
          "AND namespace_id = 'mailbox-inbox';"), ==, 4);
  close_duckdb_fixture (&duckdb);

  apply_projection_to_inbox (path, &initial);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "objects", 3);
  assert_table_count (duckdb.connection, "messages", 3);
  assert_table_count (duckdb.connection, "mailbox_memberships", 3);
  assert_materialization_checkpoint (duckdb.connection, 33, 3);
  g_assert_cmpuint (query_uint64 (duckdb.connection,
          "SELECT uidnext FROM mailbox_uid_state WHERE "
          "account_id = 'account-1' AND namespace_kind = 'mailbox' "
          "AND namespace_id = 'mailbox-inbox';"), ==, 4);
  close_duckdb_fixture (&duckdb);

  remove_catalog (path);
}

static void
test_later_record_failure_rolls_back_apply (void)
{
  g_autofree gchar *path = create_bootstrap_catalog ();
  g_auto (WyreboxDeliveryProjectionList) projection = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDeliveryMaterializer) materializer = NULL;
  TestDuckdbFixture duckdb = { 0 };

  append_projection_record (&projection, "sha256:first", 101, 1001, 11, 1);
  append_projection_record (&projection, NULL, 202, 1002, 22, 2);

  open_duckdb_fixture (path, &duckdb);
  execute_sql (duckdb.connection,
      "INSERT INTO materialization_checkpoint ("
      "checkpoint_key, journal_offset, journal_sequence"
      ") VALUES ('materialization', 99, 9);");
  close_duckdb_fixture (&duckdb);

  materializer = wyrebox_delivery_materializer_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);
  g_assert_false (wyrebox_delivery_materializer_apply_to_mailbox (materializer,
          "account-1", "mailbox-inbox", "INBOX", &projection, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  open_duckdb_fixture (path, &duckdb);
  assert_table_count (duckdb.connection, "accounts", 0);
  assert_table_count (duckdb.connection, "mailboxes", 0);
  assert_table_count (duckdb.connection, "mailbox_uid_state", 0);
  assert_table_count (duckdb.connection, "objects", 0);
  assert_table_count (duckdb.connection, "messages", 0);
  assert_table_count (duckdb.connection, "mailbox_memberships", 0);
  assert_materialization_checkpoint (duckdb.connection, 99, 9);
  close_duckdb_fixture (&duckdb);

  remove_catalog (path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/ingestion/delivery-materializer/happy-path-inbox",
      test_happy_path_materializes_inbox);
  g_test_add_func ("/ingestion/delivery-materializer/duplicate-object",
      test_duplicate_object_materializes_distinct_messages);
  g_test_add_func ("/ingestion/delivery-materializer/idempotent-reapply",
      test_reapply_projection_is_idempotent);
  g_test_add_func ("/ingestion/delivery-materializer/divergent-mailbox",
      test_divergent_mailbox_conflict_rolls_back);
  g_test_add_func ("/ingestion/delivery-materializer/divergent-object",
      test_divergent_object_conflict_rolls_back);
  g_test_add_func ("/ingestion/delivery-materializer/divergent-message",
      test_divergent_message_conflict_rolls_back);
  g_test_add_func ("/ingestion/delivery-materializer/divergent-membership",
      test_divergent_membership_conflict_rolls_back);
  g_test_add_func ("/ingestion/delivery-materializer/preserve-membership-uid",
      test_existing_membership_uid_is_preserved);
  g_test_add_func ("/ingestion/delivery-materializer/wrong-uidvalidity",
      test_wrong_uidvalidity_rolls_back);
  g_test_add_func ("/ingestion/delivery-materializer/stale-uidnext",
      test_stale_uidnext_rolls_back);
  g_test_add_func ("/ingestion/delivery-materializer/suffix-overlap",
      test_suffix_overlap_apply_preserves_existing_uids);
  g_test_add_func ("/ingestion/delivery-materializer/rollback-later-failure",
      test_later_record_failure_rolls_back_apply);

  return g_test_run ();
}
