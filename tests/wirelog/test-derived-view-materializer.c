#include "wyrebox-derived-view-materializer.h"
#include "wyrebox-derived-view-membership-changed-payload.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-schema-migration.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

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
test_membership_free (gpointer data)
{
  WyreboxWirelogDerivedMembership *membership = data;

  if (membership == NULL)
    return;

  g_free (membership->view_id);
  g_free (membership->message_id);
  g_free (membership);
}

static GPtrArray *
new_memberships (void)
{
  return g_ptr_array_new_with_free_func (test_membership_free);
}

static void
add_membership (GPtrArray *memberships, const gchar *view_id,
    const gchar *message_id)
{
  WyreboxWirelogDerivedMembership *membership = NULL;

  membership = g_new0 (WyreboxWirelogDerivedMembership, 1);
  membership->view_id = g_strdup (view_id);
  membership->message_id = g_strdup (message_id);
  g_ptr_array_add (memberships, membership);
}

static gchar *
create_catalog (void)
{
  g_autofree gchar *dir = NULL;
  g_autofree gchar *path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (WyreboxSchemaMigration) migration = NULL;

  dir = g_dir_make_tmp ("wyrebox-derived-view-materializer-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (dir);

  path = g_build_filename (dir, "catalog.duckdb", NULL);
  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  migration = wyrebox_schema_migration_new ();
  g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
          store, FALSE, &error));
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

static void
execute_sql (duckdb_connection connection, const gchar *sql)
{
  g_auto (duckdb_result) result = { 0 };

  g_assert_cmpint (duckdb_query (connection, sql, &result), ==, DuckDBSuccess);
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
seed_messages (const gchar *path)
{
  TestDuckdbFixture fixture = { 0 };

  open_duckdb_fixture (path, &fixture);
  execute_sql (fixture.connection,
      "INSERT INTO messages ("
      "message_id, account_id, object_id, journal_offset, journal_sequence"
      ") VALUES "
      "('msg-1', 'account-1', 'object-1', 1, 1),"
      "('msg-2', 'account-1', 'object-2', 2, 1),"
      "('msg-3', 'account-1', 'object-3', 3, 1),"
      "('other-account-msg', 'account-2', 'object-4', 4, 1);");
  close_duckdb_fixture (&fixture);
}

static void
seed_colon_collision_messages (const gchar *path)
{
  TestDuckdbFixture fixture = { 0 };

  open_duckdb_fixture (path, &fixture);
  execute_sql (fixture.connection,
      "INSERT INTO messages ("
      "message_id, account_id, object_id, journal_offset, journal_sequence"
      ") VALUES "
      "('c', 'account-1', 'object-c', 10, 1),"
      "('prelude', 'account-1', 'object-prelude', 11, 1),"
      "('b:c', 'account-1', 'object-b-c', 12, 1);");
  close_duckdb_fixture (&fixture);
}

static gboolean
apply_memberships (const gchar *path, GPtrArray *memberships, GError **error)
{
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, error);
  if (materializer == NULL)
    return FALSE;

  return wyrebox_derived_view_materializer_apply_memberships (materializer,
      "account-1", "view-project-alpha", "Project Alpha",
      "wirelog:project-alpha", "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaa", 1000, memberships, error);
}

static gboolean
apply_memberships_with_changes (const gchar *path,
    GPtrArray *memberships, GPtrArray **out_changes, GError **error)
{
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, error);
  if (materializer == NULL)
    return FALSE;

  return wyrebox_derived_view_materializer_apply_memberships_with_changes
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
      "wirelog:project-alpha", "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaa", 1000, memberships, out_changes, error);
}

static gboolean
apply_memberships_for_view (const gchar *path, const gchar *view_id,
    const gchar *imap_name, const gchar *definition_ref, GPtrArray *memberships,
    GError **error)
{
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, error);
  if (materializer == NULL)
    return FALSE;

  return wyrebox_derived_view_materializer_apply_memberships (materializer,
      "account-1", view_id, imap_name, definition_ref,
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      1000, memberships, error);
}

static gboolean
refresh_memberships (const gchar *path, GPtrArray *memberships, GError **error)
{
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, error);
  if (materializer == NULL)
    return FALSE;

  return wyrebox_derived_view_materializer_refresh_memberships (materializer,
      "account-1", "view-project-alpha", "Project Alpha",
      "wirelog:project-alpha", "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaa", 1000, memberships, error);
}

static gboolean
refresh_memberships_with_changes (const gchar *path,
    GPtrArray *memberships, GPtrArray **out_changes, GError **error)
{
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, error);
  if (materializer == NULL)
    return FALSE;

  return wyrebox_derived_view_materializer_refresh_memberships_with_changes
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
      "wirelog:project-alpha", "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
      "aaaaaaaaaaaaaaaaaaaaaaaa", 1000, memberships, out_changes, error);
}

static const WyreboxDerivedViewMembershipChange *
change_at (GPtrArray *changes, guint index)
{
  g_assert_nonnull (changes);
  g_assert_cmpuint (index, <, changes->len);

  return g_ptr_array_index (changes, index);
}

static void
assert_change (const WyreboxDerivedViewMembershipChange *change,
    const gchar *message_id, guint64 uid, guint64 uidvalidity,
    gboolean is_visible)
{
  g_assert_nonnull (change);
  g_assert_cmpstr (change->account_id, ==, "account-1");
  g_assert_cmpstr (change->view_id, ==, "view-project-alpha");
  g_assert_cmpstr (change->message_id, ==, message_id);
  g_assert_true (g_str_has_prefix (change->membership_id,
          "derived-view:sha256:"));
  g_assert_cmpuint (strlen (change->membership_id), ==, 84);
  g_assert_cmpstr (change->rule_version_hash, ==,
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  g_assert_cmpuint (change->uid, ==, uid);
  g_assert_cmpuint (change->uidvalidity, ==, uidvalidity);
  g_assert_cmpint (change->is_visible, ==, is_visible);
  g_assert_cmpuint (change->materialized_at_unix_us, ==, 1000);
}

static gchar *
create_journal_root (void)
{
  g_autoptr (GError) error = NULL;
  gchar *root = NULL;

  root = g_dir_make_tmp ("wyrebox-derived-view-change-journal-XXXXXX", &error);
  g_assert_no_error (error);
  g_assert_nonnull (root);

  return root;
}

static void
assert_decoded_payload_matches_change (const
    WyreboxDerivedViewMembershipChangedPayload *payload,
    const WyreboxDerivedViewMembershipChange *change)
{
  g_assert_cmpstr (payload->account_id, ==, change->account_id);
  g_assert_cmpstr (payload->view_id, ==, change->view_id);
  g_assert_cmpstr (payload->message_id, ==, change->message_id);
  g_assert_cmpstr (payload->membership_id, ==, change->membership_id);
  g_assert_cmpstr (payload->rule_version_hash, ==, change->rule_version_hash);
  g_assert_cmpuint (payload->uid, ==, change->uid);
  g_assert_cmpuint (payload->uidvalidity, ==, change->uidvalidity);
  g_assert_cmpint (payload->is_visible, ==, change->is_visible);
  g_assert_cmpuint (payload->materialized_at_unix_us, ==,
      change->materialized_at_unix_us);
}

static void
assert_journal_matches_changes (const gchar *journal_root, GPtrArray *changes)
{
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (GError) error = NULL;
  gboolean eof = FALSE;

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);

  for (guint i = 0; i < changes->len; i++) {
    const WyreboxDerivedViewMembershipChange *change = change_at (changes, i);
    g_auto (WyreboxJournalRecord) record = { 0 };
    g_auto (WyreboxDerivedViewMembershipChangedPayload) payload = { 0 };

    g_assert_true (wyrebox_journal_reader_read_next (reader, &record, &eof,
            &error));
    g_assert_no_error (error);
    g_assert_false (eof);
    g_assert_cmpuint (record.sequence, ==, i + 1);
    g_assert_cmpint (record.event_type, ==,
        WYREBOX_JOURNAL_EVENT_DERIVED_VIEW_MEMBERSHIP_CHANGED);
    g_assert_true (wyrebox_derived_view_membership_changed_payload_decode
        (record.payload, &payload, &error));
    g_assert_no_error (error);
    assert_decoded_payload_matches_change (&payload, change);
  }

  {
    g_auto (WyreboxJournalRecord) record = { 0 };

    g_assert_false (wyrebox_journal_reader_read_next (reader, &record, &eof,
            &error));
    g_assert_no_error (error);
    g_assert_true (eof);
  }
}

static void
assert_journal_empty (const gchar *journal_root)
{
  g_autoptr (WyreboxJournalReader) reader = NULL;
  g_autoptr (GError) error = NULL;
  gboolean eof = FALSE;

  reader = wyrebox_journal_reader_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (reader);
  {
    g_auto (WyreboxJournalRecord) record = { 0 };

    g_assert_false (wyrebox_journal_reader_read_next (reader, &record, &eof,
            &error));
    g_assert_no_error (error);
    g_assert_true (eof);
  }
}

static void
assert_materialized_state (const gchar *path, guint64 expected_memberships,
    guint64 expected_uidnext)
{
  TestDuckdbFixture fixture = { 0 };
  g_autofree gchar *imap_name = NULL;
  g_autofree gchar *definition_ref = NULL;

  open_duckdb_fixture (path, &fixture);
  g_assert_cmpuint (query_uint64 (fixture.connection,
          "SELECT COUNT(*) FROM accounts WHERE account_id = 'account-1';"),
      ==, 1);
  g_assert_cmpuint (query_uint64 (fixture.connection,
          "SELECT COUNT(*) FROM derived_views "
          "WHERE view_id = 'view-project-alpha' "
          "AND account_id = 'account-1' "
          "AND is_selectable = TRUE AND is_visible = TRUE;"), ==, 1);
  imap_name = query_string (fixture.connection,
      "SELECT imap_name FROM derived_views "
      "WHERE view_id = 'view-project-alpha';");
  definition_ref = query_string (fixture.connection,
      "SELECT definition_ref FROM derived_views "
      "WHERE view_id = 'view-project-alpha';");
  g_assert_cmpstr (imap_name, ==, "Project Alpha");
  g_assert_cmpstr (definition_ref, ==, "wirelog:project-alpha");
  g_assert_cmpuint (query_uint64 (fixture.connection,
          "SELECT uidnext FROM mailbox_uid_state "
          "WHERE account_id = 'account-1' "
          "AND namespace_kind = 'derived_view' "
          "AND namespace_id = 'view-project-alpha';"), ==, expected_uidnext);
  g_assert_cmpuint (query_uint64 (fixture.connection,
          "SELECT uidvalidity FROM mailbox_uid_state "
          "WHERE account_id = 'account-1' "
          "AND namespace_kind = 'derived_view' "
          "AND namespace_id = 'view-project-alpha';"), >, 0);
  g_assert_cmpuint (query_uint64 (fixture.connection,
          "SELECT uidvalidity FROM mailbox_uid_state "
          "WHERE account_id = 'account-1' "
          "AND namespace_kind = 'derived_view' "
          "AND namespace_id = 'view-project-alpha';"), <=, G_MAXUINT32);
  g_assert_cmpuint (query_uint64 (fixture.connection,
          "SELECT COUNT(*) FROM derived_view_memberships;"), ==,
      expected_memberships);
  close_duckdb_fixture (&fixture);
}

static guint64
query_uid_for_message (const gchar *path, const gchar *message_id)
{
  TestDuckdbFixture fixture = { 0 };
  g_autofree gchar *sql = NULL;
  guint64 uid = 0;

  open_duckdb_fixture (path, &fixture);
  sql = g_strdup_printf ("SELECT uid FROM derived_view_memberships "
      "WHERE view_id = 'view-project-alpha' AND message_id = '%s';",
      message_id);
  uid = query_uint64 (fixture.connection, sql);
  close_duckdb_fixture (&fixture);

  return uid;
}

static guint64
query_count (const gchar *path, const gchar *table)
{
  TestDuckdbFixture fixture = { 0 };
  g_autofree gchar *sql = NULL;
  guint64 count = 0;

  open_duckdb_fixture (path, &fixture);
  sql = g_strdup_printf ("SELECT COUNT(*) FROM %s;", table);
  count = query_uint64 (fixture.connection, sql);
  close_duckdb_fixture (&fixture);

  return count;
}

static guint64
query_membership_count_where (const gchar *path, const gchar *where_clause)
{
  TestDuckdbFixture fixture = { 0 };
  g_autofree gchar *sql = NULL;
  guint64 count = 0;

  open_duckdb_fixture (path, &fixture);
  sql = g_strdup_printf ("SELECT COUNT(*) FROM derived_view_memberships "
      "WHERE %s;", where_clause);
  count = query_uint64 (fixture.connection, sql);
  close_duckdb_fixture (&fixture);

  return count;
}

static guint64
query_uidnext (const gchar *path)
{
  TestDuckdbFixture fixture = { 0 };
  guint64 uidnext = 0;

  open_duckdb_fixture (path, &fixture);
  uidnext = query_uint64 (fixture.connection,
      "SELECT uidnext FROM mailbox_uid_state "
      "WHERE account_id = 'account-1' "
      "AND namespace_kind = 'derived_view' "
      "AND namespace_id = 'view-project-alpha';");
  close_duckdb_fixture (&fixture);

  return uidnext;
}

static guint64
query_uidvalidity_for_view (const gchar *path, const gchar *view_id)
{
  TestDuckdbFixture fixture = { 0 };
  g_autofree gchar *sql = NULL;
  guint64 uidvalidity = 0;

  open_duckdb_fixture (path, &fixture);
  sql = g_strdup_printf ("SELECT uidvalidity FROM mailbox_uid_state "
      "WHERE account_id = 'account-1' "
      "AND namespace_kind = 'derived_view' "
      "AND namespace_id = '%s';", view_id);
  uidvalidity = query_uint64 (fixture.connection, sql);
  close_duckdb_fixture (&fixture);

  return uidvalidity;
}

static guint64
query_uidvalidity (const gchar *path)
{
  return query_uidvalidity_for_view (path, "view-project-alpha");
}

static void
test_refresh_initial_snapshot_inserts_two (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) memberships = new_memberships ();
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (memberships, "view-project-alpha", "msg-1");
  add_membership (memberships, "view-project-alpha", "msg-2");

  g_assert_true (refresh_memberships (path, memberships, &error));
  g_assert_no_error (error);
  assert_materialized_state (path, 2, 3);
  g_assert_cmpuint (query_uid_for_message (path, "msg-1"), ==, 1);
  g_assert_cmpuint (query_uid_for_message (path, "msg-2"), ==, 2);
  g_assert_cmpuint (query_membership_count_where (path,
          "view_id = 'view-project-alpha' AND is_visible = TRUE"), ==, 2);

  remove_catalog (path);
}

static void
test_apply_with_changes_reports_visible_inserts (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) memberships = new_memberships ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autoptr (GError) error = NULL;
  guint64 uidvalidity = 0;

  seed_messages (path);
  add_membership (memberships, "view-project-alpha", "msg-1");
  add_membership (memberships, "view-project-alpha", "msg-2");

  g_assert_true (apply_memberships_with_changes (path, memberships, &changes,
          &error));
  g_assert_no_error (error);
  uidvalidity = query_uidvalidity (path);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 2);
  assert_change (change_at (changes, 0), "msg-1", 1, uidvalidity, TRUE);
  assert_change (change_at (changes, 1), "msg-2", 2, uidvalidity, TRUE);

  remove_catalog (path);
}

static void
test_idempotent_apply_with_changes_reports_no_changes (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) memberships = new_memberships ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (memberships, "view-project-alpha", "msg-1");

  g_assert_true (apply_memberships_with_changes (path, memberships, &changes,
          &error));
  g_assert_no_error (error);
  g_clear_pointer (&changes, g_ptr_array_unref);

  g_assert_true (apply_memberships_with_changes (path, memberships, &changes,
          &error));
  g_assert_no_error (error);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 0);

  remove_catalog (path);
}

static void
test_apply_changes_append_journal_round_trip (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autofree gchar *journal_root = create_journal_root ();
  g_autoptr (GPtrArray) memberships = new_memberships ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (memberships, "view-project-alpha", "msg-1");
  add_membership (memberships, "view-project-alpha", "msg-2");

  g_assert_true (apply_memberships_with_changes (path, memberships, &changes,
          &error));
  g_assert_no_error (error);
  g_assert_cmpuint (changes->len, ==, 2);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  g_assert_true (wyrebox_derived_view_membership_changes_append_journal
      (changes, writer, &error));
  g_assert_no_error (error);
  g_clear_object (&writer);

  assert_journal_matches_changes (journal_root, changes);

  remove_catalog (path);
  remove_tree (journal_root);
}

static void
test_refresh_removes_stale_and_preserves_retained_uid (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) initial = new_memberships ();
  g_autoptr (GPtrArray) refreshed = new_memberships ();
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (initial, "view-project-alpha", "msg-1");
  add_membership (initial, "view-project-alpha", "msg-2");
  add_membership (refreshed, "view-project-alpha", "msg-1");

  g_assert_true (refresh_memberships (path, initial, &error));
  g_assert_no_error (error);
  g_assert_true (refresh_memberships (path, refreshed, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (query_count (path, "derived_view_memberships"), ==, 2);
  g_assert_cmpuint (query_uidnext (path), ==, 3);
  g_assert_cmpuint (query_uid_for_message (path, "msg-1"), ==, 1);
  g_assert_cmpuint (query_membership_count_where (path,
          "message_id = 'msg-1' AND is_visible = TRUE"), ==, 1);
  g_assert_cmpuint (query_membership_count_where (path,
          "message_id = 'msg-2' AND is_visible = FALSE"), ==, 1);

  remove_catalog (path);
}

static void
test_refresh_with_changes_reports_hidden_stale_row (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) initial = new_memberships ();
  g_autoptr (GPtrArray) refreshed = new_memberships ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autofree gchar *original_membership_id = NULL;
  g_autoptr (GError) error = NULL;
  guint64 uidvalidity = 0;

  seed_messages (path);
  add_membership (initial, "view-project-alpha", "msg-1");
  add_membership (initial, "view-project-alpha", "msg-2");
  add_membership (refreshed, "view-project-alpha", "msg-1");

  g_assert_true (refresh_memberships_with_changes (path, initial, &changes,
          &error));
  g_assert_no_error (error);
  original_membership_id = g_strdup (change_at (changes, 1)->membership_id);
  g_clear_pointer (&changes, g_ptr_array_unref);

  g_assert_true (refresh_memberships_with_changes (path, refreshed, &changes,
          &error));
  g_assert_no_error (error);
  uidvalidity = query_uidvalidity (path);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 1);
  assert_change (change_at (changes, 0), "msg-2", 2, uidvalidity, FALSE);
  g_assert_cmpstr (change_at (changes, 0)->membership_id, ==,
      original_membership_id);

  remove_catalog (path);
}

static void
test_idempotent_changes_append_journal_writes_no_records (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autofree gchar *journal_root = create_journal_root ();
  g_autoptr (GPtrArray) memberships = new_memberships ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (memberships, "view-project-alpha", "msg-1");

  g_assert_true (apply_memberships_with_changes (path, memberships, &changes,
          &error));
  g_assert_no_error (error);
  g_clear_pointer (&changes, g_ptr_array_unref);
  g_assert_true (apply_memberships_with_changes (path, memberships, &changes,
          &error));
  g_assert_no_error (error);
  g_assert_cmpuint (changes->len, ==, 0);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  g_assert_true (wyrebox_derived_view_membership_changes_append_journal
      (changes, writer, &error));
  g_assert_no_error (error);
  g_clear_object (&writer);

  assert_journal_empty (journal_root);

  remove_catalog (path);
  remove_tree (journal_root);
}

static void
test_refresh_readding_hidden_row_restores_original_uid (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) initial = new_memberships ();
  g_autoptr (GPtrArray) removed = new_memberships ();
  g_autoptr (GPtrArray) restored = new_memberships ();
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (initial, "view-project-alpha", "msg-1");
  add_membership (initial, "view-project-alpha", "msg-2");
  add_membership (removed, "view-project-alpha", "msg-1");
  add_membership (restored, "view-project-alpha", "msg-1");
  add_membership (restored, "view-project-alpha", "msg-2");

  g_assert_true (refresh_memberships (path, initial, &error));
  g_assert_no_error (error);
  g_assert_true (refresh_memberships (path, removed, &error));
  g_assert_no_error (error);
  g_assert_true (refresh_memberships (path, restored, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (query_count (path, "derived_view_memberships"), ==, 2);
  g_assert_cmpuint (query_uidnext (path), ==, 3);
  g_assert_cmpuint (query_uid_for_message (path, "msg-1"), ==, 1);
  g_assert_cmpuint (query_uid_for_message (path, "msg-2"), ==, 2);
  g_assert_cmpuint (query_membership_count_where (path,
          "view_id = 'view-project-alpha' AND is_visible = TRUE"), ==, 2);

  remove_catalog (path);
}

static void
test_refresh_hidden_change_append_journal_round_trip (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autofree gchar *journal_root = create_journal_root ();
  g_autoptr (GPtrArray) initial = new_memberships ();
  g_autoptr (GPtrArray) refreshed = new_memberships ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GError) error = NULL;
  guint64 uidvalidity = 0;

  seed_messages (path);
  add_membership (initial, "view-project-alpha", "msg-1");
  add_membership (initial, "view-project-alpha", "msg-2");
  add_membership (refreshed, "view-project-alpha", "msg-1");

  g_assert_true (refresh_memberships_with_changes (path, initial, &changes,
          &error));
  g_assert_no_error (error);
  g_clear_pointer (&changes, g_ptr_array_unref);
  g_assert_true (refresh_memberships_with_changes (path, refreshed, &changes,
          &error));
  g_assert_no_error (error);
  uidvalidity = query_uidvalidity (path);
  g_assert_cmpuint (changes->len, ==, 1);
  assert_change (change_at (changes, 0), "msg-2", 2, uidvalidity, FALSE);

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  g_assert_true (wyrebox_derived_view_membership_changes_append_journal
      (changes, writer, &error));
  g_assert_no_error (error);
  g_clear_object (&writer);

  assert_journal_matches_changes (journal_root, changes);

  remove_catalog (path);
  remove_tree (journal_root);
}

static void
test_refresh_with_changes_reports_restore_with_original_uid (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) initial = new_memberships ();
  g_autoptr (GPtrArray) removed = new_memberships ();
  g_autoptr (GPtrArray) restored = new_memberships ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autofree gchar *original_membership_id = NULL;
  g_autoptr (GError) error = NULL;
  guint64 uidvalidity = 0;

  seed_messages (path);
  add_membership (initial, "view-project-alpha", "msg-1");
  add_membership (initial, "view-project-alpha", "msg-2");
  add_membership (removed, "view-project-alpha", "msg-1");
  add_membership (restored, "view-project-alpha", "msg-1");
  add_membership (restored, "view-project-alpha", "msg-2");

  g_assert_true (refresh_memberships_with_changes (path, initial, &changes,
          &error));
  g_assert_no_error (error);
  original_membership_id = g_strdup (change_at (changes, 1)->membership_id);
  g_clear_pointer (&changes, g_ptr_array_unref);
  g_assert_true (refresh_memberships_with_changes (path, removed, &changes,
          &error));
  g_assert_no_error (error);
  g_clear_pointer (&changes, g_ptr_array_unref);

  g_assert_true (refresh_memberships_with_changes (path, restored, &changes,
          &error));
  g_assert_no_error (error);
  uidvalidity = query_uidvalidity (path);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 1);
  assert_change (change_at (changes, 0), "msg-2", 2, uidvalidity, TRUE);
  g_assert_cmpstr (change_at (changes, 0)->membership_id, ==,
      original_membership_id);

  remove_catalog (path);
}

static void
test_refresh_new_message_after_cleanup_uses_next_uid (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) initial = new_memberships ();
  g_autoptr (GPtrArray) removed = new_memberships ();
  g_autoptr (GPtrArray) appended = new_memberships ();
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (initial, "view-project-alpha", "msg-1");
  add_membership (initial, "view-project-alpha", "msg-2");
  add_membership (removed, "view-project-alpha", "msg-1");
  add_membership (appended, "view-project-alpha", "msg-1");
  add_membership (appended, "view-project-alpha", "msg-3");

  g_assert_true (refresh_memberships (path, initial, &error));
  g_assert_no_error (error);
  g_assert_true (refresh_memberships (path, removed, &error));
  g_assert_no_error (error);
  g_assert_true (refresh_memberships (path, appended, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (query_uidnext (path), ==, 4);
  g_assert_cmpuint (query_uid_for_message (path, "msg-3"), ==, 3);
  g_assert_cmpuint (query_membership_count_where (path,
          "message_id = 'msg-2' AND is_visible = FALSE"), ==, 1);

  remove_catalog (path);
}

static void
test_refresh_with_changes_reports_new_row_after_cleanup (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) initial = new_memberships ();
  g_autoptr (GPtrArray) removed = new_memberships ();
  g_autoptr (GPtrArray) appended = new_memberships ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autoptr (GError) error = NULL;
  guint64 uidvalidity = 0;

  seed_messages (path);
  add_membership (initial, "view-project-alpha", "msg-1");
  add_membership (initial, "view-project-alpha", "msg-2");
  add_membership (removed, "view-project-alpha", "msg-1");
  add_membership (appended, "view-project-alpha", "msg-1");
  add_membership (appended, "view-project-alpha", "msg-3");

  g_assert_true (refresh_memberships_with_changes (path, initial, &changes,
          &error));
  g_assert_no_error (error);
  g_clear_pointer (&changes, g_ptr_array_unref);
  g_assert_true (refresh_memberships_with_changes (path, removed, &changes,
          &error));
  g_assert_no_error (error);
  g_clear_pointer (&changes, g_ptr_array_unref);

  g_assert_true (refresh_memberships_with_changes (path, appended, &changes,
          &error));
  g_assert_no_error (error);
  uidvalidity = query_uidvalidity (path);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 1);
  assert_change (change_at (changes, 0), "msg-3", 3, uidvalidity, TRUE);

  remove_catalog (path);
}

static void
test_refresh_other_rule_hash_is_untouched (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) first_hash = new_memberships ();
  g_autoptr (GPtrArray) second_hash = new_memberships ();
  g_autoptr (GPtrArray) empty = new_memberships ();
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (first_hash, "view-project-alpha", "msg-1");
  add_membership (second_hash, "view-project-alpha", "msg-2");

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);
  g_assert_true (wyrebox_derived_view_materializer_refresh_memberships
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
          "wirelog:project-alpha", "sha256:first", 1000, first_hash, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_derived_view_materializer_refresh_memberships
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
          "wirelog:project-alpha", "sha256:second", 1000, second_hash, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_derived_view_materializer_refresh_memberships
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
          "wirelog:project-alpha", "sha256:second", 1000, empty, &error));
  g_assert_no_error (error);
  g_clear_object (&materializer);

  g_assert_cmpuint (query_membership_count_where (path,
          "rule_version_hash = 'sha256:first' AND is_visible = TRUE"), ==, 1);
  g_assert_cmpuint (query_membership_count_where (path,
          "rule_version_hash = 'sha256:second' AND is_visible = FALSE"), ==, 1);

  remove_catalog (path);
}

static void
test_refresh_other_view_is_untouched (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) first_view = new_memberships ();
  g_autoptr (GPtrArray) second_view = new_memberships ();
  g_autoptr (GPtrArray) empty = new_memberships ();
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (first_view, "view-project-alpha", "msg-1");
  add_membership (second_view, "view-project-beta", "msg-2");

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);
  g_assert_true (wyrebox_derived_view_materializer_refresh_memberships
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
          "wirelog:project-alpha",
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          1000, first_view, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_derived_view_materializer_refresh_memberships
      (materializer, "account-1", "view-project-beta", "Project Beta",
          "wirelog:project-beta",
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          1000, second_view, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_derived_view_materializer_refresh_memberships
      (materializer, "account-1", "view-project-beta", "Project Beta",
          "wirelog:project-beta",
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          1000, empty, &error));
  g_assert_no_error (error);
  g_clear_object (&materializer);

  g_assert_cmpuint (query_membership_count_where (path,
          "view_id = 'view-project-alpha' AND is_visible = TRUE"), ==, 1);
  g_assert_cmpuint (query_membership_count_where (path,
          "view_id = 'view-project-beta' AND is_visible = FALSE"), ==, 1);

  remove_catalog (path);
}

static void
test_refresh_failure_rolls_back_visibility (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) initial = new_memberships ();
  g_autoptr (GPtrArray) invalid = new_memberships ();
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (initial, "view-project-alpha", "msg-1");
  add_membership (initial, "view-project-alpha", "msg-2");
  add_membership (invalid, "view-project-alpha", "msg-1");
  add_membership (invalid, "view-project-alpha", "missing-message");

  g_assert_true (refresh_memberships (path, initial, &error));
  g_assert_no_error (error);
  g_assert_false (refresh_memberships (path, invalid, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

  g_assert_cmpuint (query_membership_count_where (path,
          "view_id = 'view-project-alpha' AND is_visible = TRUE"), ==, 2);
  g_assert_cmpuint (query_membership_count_where (path,
          "view_id = 'view-project-alpha' AND is_visible = FALSE"), ==, 0);
  g_assert_cmpuint (query_uidnext (path), ==, 3);

  remove_catalog (path);
}

static void
test_failure_with_changes_returns_no_committed_changes (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) initial = new_memberships ();
  g_autoptr (GPtrArray) invalid = new_memberships ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (initial, "view-project-alpha", "msg-1");
  add_membership (invalid, "view-project-alpha", "missing-message");

  g_assert_true (refresh_memberships_with_changes (path, initial, &changes,
          &error));
  g_assert_no_error (error);
  g_clear_pointer (&changes, g_ptr_array_unref);

  g_assert_false (refresh_memberships_with_changes (path, invalid, &changes,
          &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_null (changes);

  remove_catalog (path);
}

static void
test_empty_refresh_hides_visible_rows_without_rewinding_uidnext (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) initial = new_memberships ();
  g_autoptr (GPtrArray) empty = new_memberships ();
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (initial, "view-project-alpha", "msg-1");
  add_membership (initial, "view-project-alpha", "msg-2");

  g_assert_true (refresh_memberships (path, initial, &error));
  g_assert_no_error (error);
  g_assert_true (refresh_memberships (path, empty, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (query_count (path, "derived_view_memberships"), ==, 2);
  g_assert_cmpuint (query_membership_count_where (path,
          "view_id = 'view-project-alpha' AND is_visible = TRUE"), ==, 0);
  g_assert_cmpuint (query_membership_count_where (path,
          "view_id = 'view-project-alpha' AND is_visible = FALSE"), ==, 2);
  g_assert_cmpuint (query_uidnext (path), ==, 3);

  remove_catalog (path);
}

static void
test_colon_join_collision_tuples_get_distinct_membership_ids (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) first = new_memberships ();
  g_autoptr (GPtrArray) prelude = new_memberships ();
  g_autoptr (GPtrArray) second = new_memberships ();
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;
  g_autoptr (GError) error = NULL;
  TestDuckdbFixture fixture = { 0 };

  seed_colon_collision_messages (path);
  add_membership (first, "a:b", "c");
  add_membership (prelude, "a", "prelude");
  add_membership (second, "a", "b:c");

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);

  g_assert_true (wyrebox_derived_view_materializer_apply_memberships
      (materializer, "account-1", "a:b", "Colon View One", "wirelog:one",
          "d", 1000, first, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_derived_view_materializer_apply_memberships
      (materializer, "account-1", "a", "Colon View Two", "wirelog:two",
          "d", 1000, prelude, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_derived_view_materializer_apply_memberships
      (materializer, "account-1", "a", "Colon View Two", "wirelog:two",
          "d", 1000, second, &error));
  g_assert_no_error (error);
  g_clear_object (&materializer);

  open_duckdb_fixture (path, &fixture);
  g_assert_cmpuint (query_uint64 (fixture.connection,
          "SELECT COUNT(*) FROM derived_view_memberships;"), ==, 3);
  g_assert_cmpuint (query_uint64 (fixture.connection,
          "SELECT COUNT(DISTINCT membership_id) "
          "FROM derived_view_memberships;"), ==, 3);
  g_assert_cmpuint (query_uint64 (fixture.connection,
          "SELECT COUNT(*) FROM derived_view_memberships "
          "WHERE membership_id LIKE 'derived-view:sha256:%' "
          "AND length(membership_id) = 84;"), ==, 3);
  g_assert_cmpuint (query_uint64 (fixture.connection,
          "SELECT uid FROM derived_view_memberships "
          "WHERE view_id = 'a:b' AND message_id = 'c';"), ==, 1);
  g_assert_cmpuint (query_uint64 (fixture.connection,
          "SELECT uid FROM derived_view_memberships "
          "WHERE view_id = 'a' AND message_id = 'b:c';"), ==, 2);
  close_duckdb_fixture (&fixture);

  remove_catalog (path);
}

static void
test_applies_two_memberships (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) memberships = new_memberships ();
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (memberships, "view-project-alpha", "msg-1");
  add_membership (memberships, "view-project-alpha", "msg-2");

  g_assert_true (apply_memberships (path, memberships, &error));
  g_assert_no_error (error);
  assert_materialized_state (path, 2, 3);
  g_assert_cmpuint (query_uid_for_message (path, "msg-1"), ==, 1);
  g_assert_cmpuint (query_uid_for_message (path, "msg-2"), ==, 2);

  remove_catalog (path);
}

static void
test_reapply_preserves_uids_and_uidnext (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) memberships = new_memberships ();
  g_autoptr (GError) error = NULL;
  guint64 before_uidnext = 0;
  guint64 before_uidvalidity = 0;

  seed_messages (path);
  add_membership (memberships, "view-project-alpha", "msg-1");
  add_membership (memberships, "view-project-alpha", "msg-2");

  g_assert_true (apply_memberships (path, memberships, &error));
  g_assert_no_error (error);
  before_uidnext = query_uidnext (path);
  before_uidvalidity = query_uidvalidity (path);
  g_assert_true (apply_memberships (path, memberships, &error));
  g_assert_no_error (error);

  assert_materialized_state (path, 2, 3);
  g_assert_cmpuint (query_uidnext (path), ==, before_uidnext);
  g_assert_cmpuint (query_uidvalidity (path), ==, before_uidvalidity);
  g_assert_cmpuint (query_uid_for_message (path, "msg-1"), ==, 1);
  g_assert_cmpuint (query_uid_for_message (path, "msg-2"), ==, 2);

  remove_catalog (path);
}

static void
test_two_views_get_distinct_nonzero_uidvalidity (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) alpha_memberships = new_memberships ();
  g_autoptr (GPtrArray) beta_memberships = new_memberships ();
  g_autoptr (GError) error = NULL;
  guint64 alpha_uidvalidity = 0;
  guint64 beta_uidvalidity = 0;

  seed_messages (path);
  add_membership (alpha_memberships, "view-project-alpha", "msg-1");
  add_membership (beta_memberships, "view-project-beta", "msg-1");

  g_assert_true (apply_memberships_for_view (path, "view-project-alpha",
          "Project Alpha", "wirelog:project-alpha", alpha_memberships, &error));
  g_assert_no_error (error);
  g_assert_true (apply_memberships_for_view (path, "view-project-beta",
          "Project Beta", "wirelog:project-beta", beta_memberships, &error));
  g_assert_no_error (error);

  alpha_uidvalidity = query_uidvalidity_for_view (path, "view-project-alpha");
  beta_uidvalidity = query_uidvalidity_for_view (path, "view-project-beta");

  g_assert_cmpuint (alpha_uidvalidity, >, 0);
  g_assert_cmpuint (alpha_uidvalidity, <=, G_MAXUINT32);
  g_assert_cmpuint (beta_uidvalidity, >, 0);
  g_assert_cmpuint (beta_uidvalidity, <=, G_MAXUINT32);
  g_assert_cmpuint (alpha_uidvalidity, !=, beta_uidvalidity);

  remove_catalog (path);
}

static void
test_refresh_preserves_uidvalidity_and_uidnext (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) memberships = new_memberships ();
  g_autoptr (GError) error = NULL;
  guint64 before_uidnext = 0;
  guint64 before_uidvalidity = 0;

  seed_messages (path);
  add_membership (memberships, "view-project-alpha", "msg-1");
  add_membership (memberships, "view-project-alpha", "msg-2");

  g_assert_true (refresh_memberships (path, memberships, &error));
  g_assert_no_error (error);
  before_uidnext = query_uidnext (path);
  before_uidvalidity = query_uidvalidity (path);
  g_assert_true (refresh_memberships (path, memberships, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (query_uidnext (path), ==, before_uidnext);
  g_assert_cmpuint (query_uidvalidity (path), ==, before_uidvalidity);

  remove_catalog (path);
}

static void
test_reopened_materializer_preserves_uidvalidity (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) memberships = new_memberships ();
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;
  g_autoptr (GError) error = NULL;
  guint64 before_uidvalidity = 0;
  guint64 before_uidnext = 0;

  seed_messages (path);
  add_membership (memberships, "view-project-alpha", "msg-1");

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);
  g_assert_true (wyrebox_derived_view_materializer_apply_memberships
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
          "wirelog:project-alpha",
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          1000, memberships, &error));
  g_assert_no_error (error);
  g_clear_object (&materializer);

  before_uidvalidity = query_uidvalidity (path);
  before_uidnext = query_uidnext (path);

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);
  g_assert_true (wyrebox_derived_view_materializer_apply_memberships
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
          "wirelog:project-alpha",
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          1000, memberships, &error));
  g_assert_no_error (error);

  g_assert_cmpuint (query_uidvalidity (path), ==, before_uidvalidity);
  g_assert_cmpuint (query_uidnext (path), ==, before_uidnext);

  remove_catalog (path);
}

static void
test_later_append_allocates_next_uid (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) initial = new_memberships ();
  g_autoptr (GPtrArray) appended = new_memberships ();
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (initial, "view-project-alpha", "msg-1");
  add_membership (initial, "view-project-alpha", "msg-2");
  add_membership (appended, "view-project-alpha", "msg-1");
  add_membership (appended, "view-project-alpha", "msg-2");
  add_membership (appended, "view-project-alpha", "msg-3");

  g_assert_true (apply_memberships (path, initial, &error));
  g_assert_no_error (error);
  g_assert_true (apply_memberships (path, appended, &error));
  g_assert_no_error (error);

  assert_materialized_state (path, 3, 4);
  g_assert_cmpuint (query_uid_for_message (path, "msg-3"), ==, 3);

  remove_catalog (path);
}

static void
assert_failed_apply_rolls_back (const gchar *path, GPtrArray *memberships)
{
  g_autoptr (GError) error = NULL;
  const guint64 before_count = query_count (path, "derived_view_memberships");
  const guint64 before_uidnext = query_uidnext (path);

  g_assert_false (apply_memberships (path, memberships, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (query_count (path, "derived_view_memberships"), ==,
      before_count);
  g_assert_cmpuint (query_uidnext (path), ==, before_uidnext);
}

static void
test_rejects_view_id_mismatch_and_rolls_back (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) valid = new_memberships ();
  g_autoptr (GPtrArray) invalid = new_memberships ();
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (valid, "view-project-alpha", "msg-1");
  add_membership (invalid, "view-project-alpha", "msg-1");
  add_membership (invalid, "other-view", "msg-2");

  g_assert_true (apply_memberships (path, valid, &error));
  g_assert_no_error (error);
  assert_failed_apply_rolls_back (path, invalid);

  remove_catalog (path);
}

static void
test_rejects_unknown_message_and_rolls_back (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) valid = new_memberships ();
  g_autoptr (GPtrArray) invalid = new_memberships ();
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (valid, "view-project-alpha", "msg-1");
  add_membership (invalid, "view-project-alpha", "msg-1");
  add_membership (invalid, "view-project-alpha", "missing-message");

  g_assert_true (apply_memberships (path, valid, &error));
  g_assert_no_error (error);
  g_assert_false (apply_memberships (path, invalid, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_assert_cmpuint (query_count (path, "derived_view_memberships"), ==, 1);
  g_assert_cmpuint (query_uidnext (path), ==, 2);

  remove_catalog (path);
}

static void
test_rejects_duplicate_input_and_rolls_back (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) valid = new_memberships ();
  g_autoptr (GPtrArray) invalid = new_memberships ();
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (valid, "view-project-alpha", "msg-1");
  add_membership (invalid, "view-project-alpha", "msg-1");
  add_membership (invalid, "view-project-alpha", "msg-1");

  g_assert_true (apply_memberships (path, valid, &error));
  g_assert_no_error (error);
  g_assert_false (apply_memberships (path, invalid, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_cmpuint (query_count (path, "derived_view_memberships"), ==, 1);
  g_assert_cmpuint (query_uidnext (path), ==, 2);

  remove_catalog (path);
}

static void
test_rejects_conflicting_derived_view_metadata_and_rolls_back (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) valid = new_memberships ();
  g_autoptr (GPtrArray) invalid = new_memberships ();
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (valid, "view-project-alpha", "msg-1");
  add_membership (invalid, "view-project-alpha", "msg-1");
  add_membership (invalid, "view-project-alpha", "msg-2");

  g_assert_true (apply_memberships (path, valid, &error));
  g_assert_no_error (error);

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);
  g_assert_false (wyrebox_derived_view_materializer_apply_memberships
      (materializer, "account-1", "view-project-alpha",
          "Different Name", "wirelog:project-alpha",
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          1000, invalid, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
  g_clear_object (&materializer);
  g_assert_cmpuint (query_count (path, "derived_view_memberships"), ==, 1);
  g_assert_cmpuint (query_uidnext (path), ==, 2);

  remove_catalog (path);
}

static void
test_invalid_arguments (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) memberships = new_memberships ();
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_membership (memberships, "view-project-alpha", "msg-1");

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);

  g_assert_false (wyrebox_derived_view_materializer_apply_memberships
      (materializer, "", "view-project-alpha", "Project Alpha",
          "wirelog:project-alpha",
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          1000, memberships, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_assert_false (wyrebox_derived_view_materializer_apply_memberships
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
          "wirelog:project-alpha",
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          0, memberships, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_assert_false (wyrebox_derived_view_materializer_apply_memberships
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
          "wirelog:project-alpha",
          "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          1000, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_clear_object (&materializer);
  g_assert_null (wyrebox_derived_view_materializer_new_duckdb ("", &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  remove_catalog (path);
}

static void
test_empty_changes_append_journal_writes_no_records (void)
{
  g_autofree gchar *journal_root = create_journal_root ();
  g_autoptr (GPtrArray) changes =
      g_ptr_array_new_with_free_func ((GDestroyNotify)
      wyrebox_derived_view_membership_change_free);
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GError) error = NULL;

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);
  g_assert_true (wyrebox_derived_view_membership_changes_append_journal
      (changes, writer, &error));
  g_assert_no_error (error);
  g_clear_object (&writer);

  assert_journal_empty (journal_root);

  remove_tree (journal_root);
}

static void
test_append_journal_invalid_arguments (void)
{
  g_autofree gchar *journal_root = create_journal_root ();
  g_autoptr (GPtrArray) changes = g_ptr_array_new ();
  g_autoptr (GPtrArray) invalid_change_array =
      g_ptr_array_new_with_free_func ((GDestroyNotify)
      wyrebox_derived_view_membership_change_free);
  g_autoptr (WyreboxJournalWriter) writer = NULL;
  g_autoptr (GError) error = NULL;
  WyreboxDerivedViewMembershipChange *invalid_change = NULL;

  writer = wyrebox_journal_writer_new (journal_root, &error);
  g_assert_no_error (error);
  g_assert_nonnull (writer);

  g_assert_false (wyrebox_derived_view_membership_changes_append_journal
      (NULL, writer, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_assert_false (wyrebox_derived_view_membership_changes_append_journal
      (changes, NULL, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  g_ptr_array_add (changes, NULL);
  g_assert_false (wyrebox_derived_view_membership_changes_append_journal
      (changes, writer, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_clear_error (&error);

  invalid_change = g_new0 (WyreboxDerivedViewMembershipChange, 1);
  invalid_change->account_id = g_strdup ("");
  invalid_change->view_id = g_strdup ("view-project-alpha");
  invalid_change->message_id = g_strdup ("msg-1");
  invalid_change->membership_id = g_strdup ("derived-view:sha256:bad");
  invalid_change->rule_version_hash = g_strdup ("sha256:bad");
  invalid_change->uid = 1;
  invalid_change->uidvalidity = 1;
  invalid_change->is_visible = TRUE;
  invalid_change->materialized_at_unix_us = 1000;
  g_ptr_array_add (invalid_change_array, invalid_change);

  g_assert_false (wyrebox_derived_view_membership_changes_append_journal
      (invalid_change_array, writer, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);

  remove_tree (journal_root);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/wirelog/derived-view-materializer/applies-two",
      test_applies_two_memberships);
  g_test_add_func ("/wirelog/derived-view-materializer/apply-changes",
      test_apply_with_changes_reports_visible_inserts);
  g_test_add_func ("/wirelog/derived-view-materializer/apply-changes-journal",
      test_apply_changes_append_journal_round_trip);
  g_test_add_func ("/wirelog/derived-view-materializer/reapply",
      test_reapply_preserves_uids_and_uidnext);
  g_test_add_func ("/wirelog/derived-view-materializer/uidvalidity-distinct",
      test_two_views_get_distinct_nonzero_uidvalidity);
  g_test_add_func ("/wirelog/derived-view-materializer/uidvalidity-refresh",
      test_refresh_preserves_uidvalidity_and_uidnext);
  g_test_add_func ("/wirelog/derived-view-materializer/uidvalidity-reopen",
      test_reopened_materializer_preserves_uidvalidity);
  g_test_add_func ("/wirelog/derived-view-materializer/reapply-changes",
      test_idempotent_apply_with_changes_reports_no_changes);
  g_test_add_func
      ("/wirelog/derived-view-materializer/reapply-changes-journal",
      test_idempotent_changes_append_journal_writes_no_records);
  g_test_add_func ("/wirelog/derived-view-materializer/append",
      test_later_append_allocates_next_uid);
  g_test_add_func ("/wirelog/derived-view-materializer/colon-collision",
      test_colon_join_collision_tuples_get_distinct_membership_ids);
  g_test_add_func ("/wirelog/derived-view-materializer/refresh-initial",
      test_refresh_initial_snapshot_inserts_two);
  g_test_add_func ("/wirelog/derived-view-materializer/refresh-removes-stale",
      test_refresh_removes_stale_and_preserves_retained_uid);
  g_test_add_func
      ("/wirelog/derived-view-materializer/refresh-removes-stale-changes",
      test_refresh_with_changes_reports_hidden_stale_row);
  g_test_add_func
      ("/wirelog/derived-view-materializer/refresh-stale-changes-journal",
      test_refresh_hidden_change_append_journal_round_trip);
  g_test_add_func ("/wirelog/derived-view-materializer/refresh-restores",
      test_refresh_readding_hidden_row_restores_original_uid);
  g_test_add_func ("/wirelog/derived-view-materializer/refresh-restore-changes",
      test_refresh_with_changes_reports_restore_with_original_uid);
  g_test_add_func ("/wirelog/derived-view-materializer/refresh-next-uid",
      test_refresh_new_message_after_cleanup_uses_next_uid);
  g_test_add_func
      ("/wirelog/derived-view-materializer/refresh-next-uid-changes",
      test_refresh_with_changes_reports_new_row_after_cleanup);
  g_test_add_func ("/wirelog/derived-view-materializer/refresh-rule-scope",
      test_refresh_other_rule_hash_is_untouched);
  g_test_add_func ("/wirelog/derived-view-materializer/refresh-view-scope",
      test_refresh_other_view_is_untouched);
  g_test_add_func ("/wirelog/derived-view-materializer/refresh-rollback",
      test_refresh_failure_rolls_back_visibility);
  g_test_add_func ("/wirelog/derived-view-materializer/failure-changes",
      test_failure_with_changes_returns_no_committed_changes);
  g_test_add_func ("/wirelog/derived-view-materializer/refresh-empty",
      test_empty_refresh_hides_visible_rows_without_rewinding_uidnext);
  g_test_add_func ("/wirelog/derived-view-materializer/view-mismatch",
      test_rejects_view_id_mismatch_and_rolls_back);
  g_test_add_func ("/wirelog/derived-view-materializer/unknown-message",
      test_rejects_unknown_message_and_rolls_back);
  g_test_add_func ("/wirelog/derived-view-materializer/duplicate-input",
      test_rejects_duplicate_input_and_rolls_back);
  g_test_add_func ("/wirelog/derived-view-materializer/conflicting-view",
      test_rejects_conflicting_derived_view_metadata_and_rolls_back);
  g_test_add_func ("/wirelog/derived-view-materializer/invalid-arguments",
      test_invalid_arguments);
  g_test_add_func ("/wirelog/derived-view-materializer/empty-changes-journal",
      test_empty_changes_append_journal_writes_no_records);
  g_test_add_func ("/wirelog/derived-view-materializer/invalid-journal-args",
      test_append_journal_invalid_arguments);

  return g_test_run ();
}
