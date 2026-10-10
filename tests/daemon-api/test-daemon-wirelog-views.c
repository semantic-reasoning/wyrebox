#include "wyrebox-daemon-config.h"
#include "wyrebox-daemon-wirelog-views.h"
#include "wyrebox-duckdb-shared.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-schema-metadata-store.h"
#include "wyrebox-schema-migration.h"
#include "wyrebox-wirelog-derived-membership.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#define TEST_RULES \
        ".decl has_keyword(message_id: symbol, keyword: symbol)\n" \
        ".decl show_in_virtual_folder(view_id: symbol, message_id: symbol)\n" \
        "show_in_virtual_folder(\"projects\", message_id) :- " \
        "has_keyword(message_id, \"project\").\n" \
        "show_in_virtual_folder(\"ops\", message_id) :- " \
        "has_keyword(message_id, \"ops\").\n"

typedef struct
{
    gchar *root;
    gchar *catalog_path;
    gchar *rules_path;
    WyreboxJournalWriter *writer;
    duckdb_database database;
    duckdb_connection connection;
} ViewsFixture;

static void
duckdb_result_clear (duckdb_result *result)
{
    duckdb_destroy_result (result);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_result, duckdb_result_clear)

static void
remove_tree (const gchar *path)
{
    g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
    const gchar *name = NULL;

    if (dir == NULL) {
        (void)g_remove (path);
        return;
    }

    while ((name = g_dir_read_name (dir)) != NULL) {
        g_autofree gchar *child = g_build_filename (path, name, NULL);

        remove_tree (child);
    }
    (void)g_rmdir (path);
}

static void
execute_sql (ViewsFixture *fixture, const gchar *sql)
{
    g_auto (duckdb_result) result = { 0 };

    if (duckdb_query (fixture->connection, sql, &result) != DuckDBSuccess)
        g_error ("%s: %s", sql, duckdb_result_error (&result));
}

static gchar *
query_rows (ViewsFixture *fixture, const gchar *sql)
{
    g_auto (duckdb_result) result = { 0 };
    g_autoptr (GString) rows = g_string_new (NULL);

    g_assert_cmpint (duckdb_query (fixture->connection, sql, &result), ==,
        DuckDBSuccess);
    for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
        if (row > 0)
            g_string_append (rows, "; ");
        for (idx_t column = 0; column < duckdb_column_count (&result);
            column++) {
            char *value = duckdb_value_varchar (&result, column, row);

            if (column > 0)
                g_string_append_c (rows, ',');
            g_string_append (rows, value);
            duckdb_free (value);
        }
    }

    return g_string_free (g_steal_pointer (&rows), FALSE);
}

static gchar *
visible_memberships (ViewsFixture *fixture)
{
    return query_rows (fixture,
               "SELECT view_id, message_id, uid FROM derived_view_memberships "
               "WHERE account_id = 'account-1' AND is_visible "
               "ORDER BY view_id, uid;");
}

static void
write_rules (ViewsFixture *fixture, const gchar *rules)
{
    g_autoptr (GError) error = NULL;

    g_assert_true (g_file_set_contents (fixture->rules_path, rules, -1,
        &error));
    g_assert_no_error (error);
}

static void
insert_fact (ViewsFixture *fixture, const gchar *fact_id,
    const gchar *message_id, const gchar *predicate, const gchar *args_json)
{
    g_autofree gchar *sql = g_strdup_printf ("INSERT INTO message_facts ("
            "fact_id, account_id, message_id, object_id, predicate, args_json, "
            "source, confidence_ppm, created_at_unix_us, retracted_at_unix_us, "
            "journal_offset, journal_sequence) VALUES ('%s', 'account-1', "
            "'%s', '', '%s', '%s', 'fact-mutation:account-1', 1000000, 1, 0, "
            "1, 1);", fact_id, message_id, predicate, args_json);

    execute_sql (fixture, sql);
}

static void
views_fixture_set_up (ViewsFixture *fixture, gconstpointer user_data)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
    g_autoptr (WyreboxSchemaMigration) migration = NULL;
    g_autofree gchar *journal_root = NULL;

    (void)user_data;

    fixture->root = g_dir_make_tmp ("wyrebox-wirelog-views-XXXXXX", &error);
    g_assert_no_error (error);
    fixture->catalog_path = g_build_filename (fixture->root, "catalog.duckdb",
            NULL);
    fixture->rules_path = g_build_filename (fixture->root, "views.dl", NULL);
    journal_root = g_build_filename (fixture->root, "journal", NULL);
    g_assert_cmpint (g_mkdir (journal_root, 0700), ==, 0);
    fixture->writer = wyrebox_journal_writer_new (journal_root, &error);
    g_assert_no_error (error);

    store = wyrebox_schema_metadata_store_new_duckdb (fixture->catalog_path,
            &error);
    g_assert_no_error (error);
    migration = wyrebox_schema_migration_new ();
    g_assert_true (wyrebox_schema_migration_run_store_to_current (migration,
        store, FALSE, &error));
    g_assert_no_error (error);

    g_assert_true (wyrebox_duckdb_open_shared (fixture->catalog_path,
        &fixture->database, &error));
    g_assert_no_error (error);
    g_assert_cmpint (duckdb_connect (fixture->database, &fixture->connection),
        ==, DuckDBSuccess);
    execute_sql (fixture,
        "INSERT INTO accounts (account_id) VALUES ('account-1'), "
        "('account-2');");
    execute_sql (fixture,
        "INSERT INTO messages ("
        "message_id, account_id, object_id, journal_offset, journal_sequence"
        ") VALUES "
        "('msg-1', 'account-1', 'object-1', 1, 1),"
        "('msg-2', 'account-1', 'object-2', 2, 2),"
        "('msg-3', 'account-1', 'object-3', 3, 3);");
    write_rules (fixture, TEST_RULES);
}

static void
views_fixture_tear_down (ViewsFixture *fixture, gconstpointer user_data)
{
    (void)user_data;

    duckdb_disconnect (&fixture->connection);
    duckdb_close (&fixture->database);
    g_clear_object (&fixture->writer);
    remove_tree (fixture->root);
    g_free (fixture->root);
    g_free (fixture->catalog_path);
    g_free (fixture->rules_path);
}

static WyreboxDaemonWirelogViews *
open_views (ViewsFixture *fixture)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxDaemonWirelogViews) views = NULL;

    views = wyrebox_daemon_wirelog_views_new (fixture->rules_path, &error);
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_wirelog_views_add_view (views, "projects",
        "Projects", &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_wirelog_views_add_view (views, "ops", "Ops",
        &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_wirelog_views_open_catalog (views,
        fixture->catalog_path, fixture->writer, &error));
    g_assert_no_error (error);

    return g_steal_pointer (&views);
}

static void
refresh (WyreboxDaemonWirelogViews *views, const gchar *account_id)
{
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_wirelog_views_refresh_account (views,
        account_id, &error));
    g_assert_no_error (error);
}

static void
test_refresh_derives_views_from_materialized_facts (ViewsFixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (WyreboxDaemonWirelogViews) views = open_views (fixture);
    g_autofree gchar *initial = NULL;
    g_autofree gchar *grown = NULL;
    g_autofree gchar *shrunk = NULL;
    g_autofree gchar *views_rows = NULL;

    (void)user_data;

    insert_fact (fixture, "f1", "msg-1", "has_keyword",
        "[\"msg-1\",\"project\"]");
    insert_fact (fixture, "f2", "msg-2", "has_keyword", "[\"msg-2\",\"ops\"]");
    insert_fact (fixture, "f3", "missing", "has_keyword",
        "[\"missing\",\"project\"]");
    insert_fact (fixture, "f4", "msg-3", "undeclared_predicate",
        "[\"msg-3\"]");
    refresh (views, "account-1");

    initial = visible_memberships (fixture);
    g_assert_cmpstr (initial, ==, "ops,msg-2,1; projects,msg-1,1");
    views_rows = query_rows (fixture,
            "SELECT view_id, imap_name, definition_ref FROM derived_views "
            "WHERE account_id = 'account-1' ORDER BY view_id;");
    g_assert_cmpstr (views_rows, ==,
        "ops,Ops,wirelog:ops; projects,Projects,wirelog:projects");

    insert_fact (fixture, "f5", "msg-3", "has_keyword",
        "[\"msg-3\",\"project\"]");
    refresh (views, "account-1");
    grown = visible_memberships (fixture);
    g_assert_cmpstr (grown, ==,
        "ops,msg-2,1; projects,msg-1,1; projects,msg-3,2");

    execute_sql (fixture,
        "UPDATE message_facts SET retracted_at_unix_us = 9 "
        "WHERE fact_id = 'f1';");
    refresh (views, "account-1");
    shrunk = visible_memberships (fixture);
    g_assert_cmpstr (shrunk, ==, "ops,msg-2,1; projects,msg-3,2");
}

static void
test_refresh_without_facts_creates_empty_views (ViewsFixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (WyreboxDaemonWirelogViews) views = open_views (fixture);
    g_autoptr (GError) error = NULL;
    g_auto (GStrv) accounts = NULL;
    g_autofree gchar *views_rows = NULL;

    (void)user_data;

    accounts = wyrebox_daemon_wirelog_views_list_accounts (views, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (g_strv_length (accounts), ==, 2);
    g_assert_cmpstr (accounts[0], ==, "account-1");
    g_assert_cmpstr (accounts[1], ==, "account-2");

    refresh (views, "account-2");
    views_rows = query_rows (fixture,
            "SELECT view_id FROM derived_views WHERE account_id = 'account-2' "
            "ORDER BY view_id;");
    g_assert_cmpstr (views_rows, ==, "ops; projects");
}

static void
test_evaluate_decodes_escaped_arguments (ViewsFixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (WyreboxDaemonWirelogViews) views = open_views (fixture);
    g_autoptr (GPtrArray) memberships = NULL;
    g_autoptr (GError) error = NULL;
    const WyreboxWirelogDerivedMembership *membership = NULL;

    (void)user_data;

    insert_fact (fixture, "f1", "msg-1", "has_keyword",
        "[\"msg\\/\\u00e9\",\"project\"]");

    memberships = wyrebox_daemon_wirelog_views_evaluate (views, "account-1",
            WYREBOX_DAEMON_WIRELOG_VIEWS_MEMBERSHIP_RELATION, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (memberships->len, ==, 1);
    membership = g_ptr_array_index (memberships, 0);
    g_assert_cmpstr (membership->view_id, ==, "projects");
    g_assert_cmpstr (membership->message_id, ==, "msg/\xc3\xa9");
}

static void
test_evaluate_rejects_malformed_arguments (ViewsFixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (WyreboxDaemonWirelogViews) views = open_views (fixture);
    g_autoptr (GPtrArray) memberships = NULL;
    g_autoptr (GError) error = NULL;

    (void)user_data;

    insert_fact (fixture, "f1", "msg-1", "has_keyword", "[\"msg-1\",3]");

    memberships = wyrebox_daemon_wirelog_views_evaluate (views, "account-1",
            WYREBOX_DAEMON_WIRELOG_VIEWS_MEMBERSHIP_RELATION, &error);
    g_assert_null (memberships);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
test_new_rejects_unusable_rules (ViewsFixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (WyreboxDaemonWirelogViews) views = NULL;
    g_autoptr (GError) error = NULL;
    g_autofree gchar *missing = g_build_filename (fixture->root, "missing.dl",
            NULL);

    (void)user_data;

    views = wyrebox_daemon_wirelog_views_new (missing, &error);
    g_assert_null (views);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "cannot be read"));
    g_clear_error (&error);

    write_rules (fixture, "show_in_virtual_folder(");
    views = wyrebox_daemon_wirelog_views_new (fixture->rules_path, &error);
    g_assert_null (views);
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "does not compile"));
    g_clear_error (&error);

    write_rules (fixture,
        ".decl has_keyword(message_id: symbol, keyword: symbol)\n");
    views = wyrebox_daemon_wirelog_views_new (fixture->rules_path, &error);
    g_assert_no_error (error);
    g_assert_false (wyrebox_daemon_wirelog_views_add_view (views, "projects",
        "Projects", &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "show_in_virtual_folder"));
}

static void
test_example_config_loads (void)
{
    const char *examples_dir = g_getenv ("WYREBOX_EXAMPLES_DIR");
    g_autofree gchar *config_path = NULL;
    g_autofree gchar *rules_path = NULL;
    g_autoptr (WyreboxDaemonConfig) config = NULL;
    g_autoptr (WyreboxDaemonWirelogViews) views = NULL;
    g_autoptr (GError) error = NULL;

    g_assert_nonnull (examples_dir);
    config_path = g_build_filename (examples_dir, "wyrebox.conf", NULL);
    rules_path = g_build_filename (examples_dir, "views.dl", NULL);

    config = wyrebox_daemon_config_new_from_file (config_path, &error);
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_config_validate_for_startup (config,
        &error));
    g_assert_no_error (error);
    g_assert_cmpuint (wyrebox_daemon_config_get_n_views (config), ==, 1);

    views = wyrebox_daemon_wirelog_views_new (rules_path, &error);
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_wirelog_views_add_view (views,
        wyrebox_daemon_config_get_view_id (config, 0),
        wyrebox_daemon_config_get_view_imap_name (config, 0), &error));
    g_assert_no_error (error);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add ("/daemon-api/wirelog-views/refresh-from-facts", ViewsFixture,
        NULL, views_fixture_set_up,
        test_refresh_derives_views_from_materialized_facts,
        views_fixture_tear_down);
    g_test_add ("/daemon-api/wirelog-views/refresh-without-facts",
        ViewsFixture, NULL, views_fixture_set_up,
        test_refresh_without_facts_creates_empty_views,
        views_fixture_tear_down);
    g_test_add ("/daemon-api/wirelog-views/evaluate-escaped-arguments",
        ViewsFixture, NULL, views_fixture_set_up,
        test_evaluate_decodes_escaped_arguments, views_fixture_tear_down);
    g_test_add ("/daemon-api/wirelog-views/evaluate-malformed-arguments",
        ViewsFixture, NULL, views_fixture_set_up,
        test_evaluate_rejects_malformed_arguments, views_fixture_tear_down);
    g_test_add ("/daemon-api/wirelog-views/new-rejects-unusable-rules",
        ViewsFixture, NULL, views_fixture_set_up,
        test_new_rejects_unusable_rules, views_fixture_tear_down);
    g_test_add_func ("/daemon-api/wirelog-views/example-config-loads",
        test_example_config_loads);

    return g_test_run ();
}
