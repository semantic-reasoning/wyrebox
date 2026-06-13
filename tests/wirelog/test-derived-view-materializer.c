#include "wyrebox-derived-view-materializer.h"
#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-schema-migration.h"

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
          "AND namespace_id = 'view-project-alpha';"), ==, 1);
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

  seed_messages (path);
  add_membership (memberships, "view-project-alpha", "msg-1");
  add_membership (memberships, "view-project-alpha", "msg-2");

  g_assert_true (apply_memberships (path, memberships, &error));
  g_assert_no_error (error);
  g_assert_true (apply_memberships (path, memberships, &error));
  g_assert_no_error (error);

  assert_materialized_state (path, 2, 3);
  g_assert_cmpuint (query_uid_for_message (path, "msg-1"), ==, 1);
  g_assert_cmpuint (query_uid_for_message (path, "msg-2"), ==, 2);

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

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/wirelog/derived-view-materializer/applies-two",
      test_applies_two_memberships);
  g_test_add_func ("/wirelog/derived-view-materializer/reapply",
      test_reapply_preserves_uids_and_uidnext);
  g_test_add_func ("/wirelog/derived-view-materializer/append",
      test_later_append_allocates_next_uid);
  g_test_add_func ("/wirelog/derived-view-materializer/colon-collision",
      test_colon_join_collision_tuples_get_distinct_membership_ids);
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

  return g_test_run ();
}
