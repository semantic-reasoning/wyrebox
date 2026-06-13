#include "wyrebox-derived-view-materializer-wirelog.h"
#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-schema-migration.h"
#include "wyrebox-wirelog-rule-version.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

typedef struct
{
  duckdb_database database;
  duckdb_connection connection;
} TestDuckdbFixture;

static const gchar *membership_rules =
    ".decl project_keyword(message_id: symbol, view_id: symbol)\n"
    ".decl show_in_virtual_folder(view_id: symbol, message_id: symbol)\n"
    "show_in_virtual_folder(view_id, message_id) :- "
    "project_keyword(message_id, view_id).\n";

static void
remove_tree (const gchar *path)
{
  g_autoptr (GDir) dir = NULL;
  const gchar *name = NULL;

  dir = g_dir_open (path, 0, NULL);
  if (dir == NULL) {
    (void) g_remove (path);
    return;
  }

  while ((name = g_dir_read_name (dir)) != NULL) {
    g_autofree gchar *child = g_build_filename (path, name, NULL);

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
create_catalog (void)
{
  g_autofree gchar *dir = NULL;
  g_autofree gchar *path = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
  g_autoptr (WyreboxSchemaMigration) migration = NULL;

  dir = g_dir_make_tmp ("wyrebox-derived-view-wirelog-XXXXXX", &error);
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
      "('msg-3', 'account-1', 'object-3', 3, 1);");
  close_duckdb_fixture (&fixture);
}

static void
fact_record_free (gpointer data)
{
  WyreboxFactRecord *record = data;

  if (record == NULL)
    return;

  wyrebox_fact_record_clear (record);
  g_free (record);
}

static GPtrArray *
new_fact_array (void)
{
  return g_ptr_array_new_with_free_func (fact_record_free);
}

static void
add_project_fact (GPtrArray *facts, const gchar *message_id,
    const gchar *view_id)
{
  const gchar *args[] = {
    message_id,
    view_id,
    NULL,
  };
  g_autoptr (GError) error = NULL;
  WyreboxFactRecord *record = g_new0 (WyreboxFactRecord, 1);

  g_assert_true (wyrebox_fact_record_init (record,
          "project_keyword", args, "test:wirelog-bridge", 1000000, 100,
          &error));
  g_assert_no_error (error);
  g_ptr_array_add (facts, record);
}

static gboolean
refresh_from_facts (const gchar *path,
    GPtrArray *facts, GPtrArray **out_changes, GError **error)
{
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, error);
  if (materializer == NULL)
    return FALSE;

  return
      wyrebox_derived_view_materializer_refresh_from_rules_and_facts_with_changes
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
      "wirelog:project-alpha", 1000, membership_rules, facts,
      "show_in_virtual_folder", out_changes, error);
}

static guint64
query_membership_count (const gchar *path, const gchar *where_clause)
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
query_membership_count_for_hash (const gchar *path,
    const gchar *message_id, const gchar *rule_hash)
{
  g_autofree gchar *where_clause = NULL;

  where_clause = g_strdup_printf ("message_id = '%s' "
      "AND rule_version_hash = '%s'", message_id, rule_hash);
  return query_membership_count (path, where_clause);
}

static guint64
query_membership_count_for_hash_visible (const gchar *path,
    const gchar *message_id, const gchar *rule_hash, gboolean is_visible)
{
  g_autofree gchar *where_clause = NULL;

  where_clause = g_strdup_printf ("message_id = '%s' "
      "AND rule_version_hash = '%s' AND is_visible = %s", message_id,
      rule_hash, is_visible ? "TRUE" : "FALSE");
  return query_membership_count (path, where_clause);
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

static gchar *
query_rule_hash_for_message (const gchar *path, const gchar *message_id)
{
  TestDuckdbFixture fixture = { 0 };
  g_autofree gchar *sql = NULL;
  g_auto (duckdb_result) result = { 0 };
  gchar *duckdb_value = NULL;
  gchar *hash = NULL;

  open_duckdb_fixture (path, &fixture);
  sql = g_strdup_printf ("SELECT rule_version_hash "
      "FROM derived_view_memberships "
      "WHERE view_id = 'view-project-alpha' AND message_id = '%s' "
      "ORDER BY uid LIMIT 1;", message_id);
  g_assert_cmpint (duckdb_query (fixture.connection, sql, &result), ==,
      DuckDBSuccess);
  g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
  g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
  g_assert_false (duckdb_value_is_null (&result, 0, 0));
  duckdb_value = duckdb_value_varchar (&result, 0, 0);
  hash = g_strdup (duckdb_value);
  duckdb_free (duckdb_value);
  close_duckdb_fixture (&fixture);

  return hash;
}

static const WyreboxDerivedViewMembershipChange *
change_at (GPtrArray *changes, guint index)
{
  g_assert_nonnull (changes);
  g_assert_cmpuint (index, <, changes->len);

  return g_ptr_array_index (changes, index);
}

static void
test_bridge_refreshes_sorted_memberships (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autofree gchar *expected_hash = NULL;
  g_autofree gchar *stored_hash = NULL;
  g_autoptr (GPtrArray) facts = new_fact_array ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_project_fact (facts, "msg-2", "view-project-alpha");
  add_project_fact (facts, "msg-1", "view-project-alpha");
  expected_hash = wyrebox_wirelog_rule_version_hash (membership_rules, &error);
  g_assert_no_error (error);
  g_assert_nonnull (expected_hash);

  g_assert_true (refresh_from_facts (path, facts, &changes, &error));
  g_assert_no_error (error);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 2);
  g_assert_cmpstr (change_at (changes, 0)->message_id, ==, "msg-1");
  g_assert_cmpuint (change_at (changes, 0)->uid, ==, 1);
  g_assert_cmpstr (change_at (changes, 1)->message_id, ==, "msg-2");
  g_assert_cmpuint (change_at (changes, 1)->uid, ==, 2);
  g_assert_cmpuint (query_uid_for_message (path, "msg-1"), ==, 1);
  g_assert_cmpuint (query_uid_for_message (path, "msg-2"), ==, 2);
  g_assert_cmpuint (query_membership_count (path,
          "view_id = 'view-project-alpha' AND is_visible = TRUE"), ==, 2);
  stored_hash = query_rule_hash_for_message (path, "msg-1");
  g_assert_cmpstr (stored_hash, ==, expected_hash);
  g_assert_cmpstr (change_at (changes, 0)->rule_version_hash, ==,
      expected_hash);

  remove_catalog (path);
}

static void
test_bridge_refresh_hides_removed_membership (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) initial = new_fact_array ();
  g_autoptr (GPtrArray) refreshed = new_fact_array ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_project_fact (initial, "msg-2", "view-project-alpha");
  add_project_fact (initial, "msg-1", "view-project-alpha");
  add_project_fact (refreshed, "msg-1", "view-project-alpha");

  g_assert_true (refresh_from_facts (path, initial, &changes, &error));
  g_assert_no_error (error);
  g_clear_pointer (&changes, g_ptr_array_unref);

  g_assert_true (refresh_from_facts (path, refreshed, &changes, &error));
  g_assert_no_error (error);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 1);
  g_assert_cmpstr (change_at (changes, 0)->message_id, ==, "msg-2");
  g_assert_cmpuint (change_at (changes, 0)->uid, ==, 2);
  g_assert_false (change_at (changes, 0)->is_visible);
  g_assert_cmpuint (query_membership_count (path,
          "message_id = 'msg-1' AND is_visible = TRUE"), ==, 1);
  g_assert_cmpuint (query_membership_count (path,
          "message_id = 'msg-2' AND is_visible = FALSE"), ==, 1);
  g_assert_cmpuint (query_uidnext (path), ==, 3);

  remove_catalog (path);
}

static void
test_bridge_unknown_rule_symbol_leaves_state_unchanged (void)
{
  const gchar *unknown_symbol_rules =
      ".decl project_keyword(message_id: symbol, view_id: symbol)\n"
      ".decl show_in_virtual_folder(view_id: symbol, message_id: symbol)\n"
      "show_in_virtual_folder(\"view-from-rule\", message_id) :- "
      "project_keyword(message_id, view_id).\n";
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) initial = new_fact_array ();
  g_autoptr (GPtrArray) invalid = new_fact_array ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_project_fact (initial, "msg-1", "view-project-alpha");
  add_project_fact (initial, "msg-2", "view-project-alpha");
  add_project_fact (invalid, "msg-3", "view-project-alpha");

  g_assert_true (refresh_from_facts (path, initial, &changes, &error));
  g_assert_no_error (error);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 2);

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);
  g_assert_false
      (wyrebox_derived_view_materializer_refresh_from_rules_and_facts_with_changes
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
          "wirelog:project-alpha", 1000, unknown_symbol_rules, invalid,
          "show_in_virtual_folder", &changes, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_assert_null (changes);
  g_clear_object (&materializer);

  g_assert_cmpuint (query_membership_count (path,
          "view_id = 'view-project-alpha' AND is_visible = TRUE"), ==, 2);
  g_assert_cmpuint (query_membership_count (path,
          "message_id = 'msg-3'"), ==, 0);
  g_assert_cmpuint (query_uidnext (path), ==, 3);

  remove_catalog (path);
}

static void
test_bridge_changed_rules_use_distinct_hashes (void)
{
  const gchar *alternate_rules =
      ".decl project_keyword(message_id: symbol, view_id: symbol)\n"
      ".decl show_in_virtual_folder(view_id: symbol, message_id: symbol)\n"
      "show_in_virtual_folder(view_id, message_id) :- "
      "project_keyword(message_id, view_id).\n\n";
  g_autofree gchar *path = create_catalog ();
  g_autofree gchar *first_hash = NULL;
  g_autofree gchar *second_hash = NULL;
  g_autoptr (GPtrArray) facts = new_fact_array ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_project_fact (facts, "msg-2", "view-project-alpha");
  add_project_fact (facts, "msg-1", "view-project-alpha");
  first_hash = wyrebox_wirelog_rule_version_hash (membership_rules, &error);
  g_assert_no_error (error);
  second_hash = wyrebox_wirelog_rule_version_hash (alternate_rules, &error);
  g_assert_no_error (error);
  g_assert_cmpstr (first_hash, !=, second_hash);

  g_assert_true (refresh_from_facts (path, facts, &changes, &error));
  g_assert_no_error (error);
  g_clear_pointer (&changes, g_ptr_array_unref);

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);
  g_assert_true
      (wyrebox_derived_view_materializer_refresh_from_rules_and_facts_with_changes
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
          "wirelog:project-alpha", 1000, alternate_rules, facts,
          "show_in_virtual_folder", &changes, &error));
  g_assert_no_error (error);
  g_assert_nonnull (changes);
  g_assert_cmpuint (changes->len, ==, 4);
  g_assert_cmpstr (change_at (changes, 0)->message_id, ==, "msg-1");
  g_assert_cmpstr (change_at (changes, 0)->rule_version_hash, ==, second_hash);
  g_assert_true (change_at (changes, 0)->is_visible);
  g_assert_cmpstr (change_at (changes, 1)->message_id, ==, "msg-2");
  g_assert_cmpstr (change_at (changes, 1)->rule_version_hash, ==, second_hash);
  g_assert_true (change_at (changes, 1)->is_visible);
  g_assert_cmpstr (change_at (changes, 2)->message_id, ==, "msg-1");
  g_assert_cmpstr (change_at (changes, 2)->rule_version_hash, ==, first_hash);
  g_assert_false (change_at (changes, 2)->is_visible);
  g_assert_cmpstr (change_at (changes, 3)->message_id, ==, "msg-2");
  g_assert_cmpstr (change_at (changes, 3)->rule_version_hash, ==, first_hash);
  g_assert_false (change_at (changes, 3)->is_visible);
  g_clear_object (&materializer);

  g_assert_cmpuint (query_membership_count (path,
          "message_id = 'msg-1'"), ==, 2);
  g_assert_cmpuint (query_membership_count_for_hash (path, "msg-1",
          first_hash), ==, 1);
  g_assert_cmpuint (query_membership_count_for_hash (path, "msg-1",
          second_hash), ==, 1);
  g_assert_cmpuint (query_membership_count_for_hash_visible (path, "msg-1",
          first_hash, FALSE), ==, 1);
  g_assert_cmpuint (query_membership_count_for_hash_visible (path, "msg-1",
          first_hash, TRUE), ==, 0);
  g_assert_cmpuint (query_membership_count_for_hash_visible (path, "msg-1",
          second_hash, TRUE), ==, 1);
  g_assert_cmpuint (query_membership_count (path,
          "view_id = 'view-project-alpha' AND is_visible = TRUE"), ==, 2);

  remove_catalog (path);
}

static void
test_bridge_empty_rules_fail_before_materialization (void)
{
  g_autofree gchar *path = create_catalog ();
  g_autoptr (GPtrArray) facts = new_fact_array ();
  g_autoptr (GPtrArray) changes = NULL;
  g_autoptr (WyreboxDerivedViewMaterializer) materializer = NULL;
  g_autoptr (GError) error = NULL;

  seed_messages (path);
  add_project_fact (facts, "msg-1", "view-project-alpha");

  materializer = wyrebox_derived_view_materializer_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (materializer);
  g_assert_false
      (wyrebox_derived_view_materializer_refresh_from_rules_and_facts_with_changes
      (materializer, "account-1", "view-project-alpha", "Project Alpha",
          "wirelog:project-alpha", 1000, "", facts, "show_in_virtual_folder",
          &changes, &error));
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
  g_assert_null (changes);
  g_clear_object (&materializer);

  g_assert_cmpuint (query_membership_count (path,
          "view_id = 'view-project-alpha'"), ==, 0);

  remove_catalog (path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/wirelog/derived-view-materializer-bridge/refresh-sorted",
      test_bridge_refreshes_sorted_memberships);
  g_test_add_func ("/wirelog/derived-view-materializer-bridge/refresh-hides",
      test_bridge_refresh_hides_removed_membership);
  g_test_add_func
      ("/wirelog/derived-view-materializer-bridge/unknown-symbol-unchanged",
      test_bridge_unknown_rule_symbol_leaves_state_unchanged);
  g_test_add_func ("/wirelog/derived-view-materializer-bridge/rule-hash-change",
      test_bridge_changed_rules_use_distinct_hashes);
  g_test_add_func ("/wirelog/derived-view-materializer-bridge/empty-rules",
      test_bridge_empty_rules_fail_before_materialization);

  return g_test_run ();
}
