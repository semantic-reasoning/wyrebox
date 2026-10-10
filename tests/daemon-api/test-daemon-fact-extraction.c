#include "wyrebox-daemon-delivery-materialization.h"
#include "wyrebox-daemon-fact-extraction.h"
#include "wyrebox-daemon-runtime.h"
#include "wyrebox-duckdb-shared.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>

typedef struct
{
    gchar *root;
    gchar *journal_root;
    gchar *catalog_path;
    gchar *rules_path;
    WyreboxLocalObjectStore *object_store;
    WyreboxJournalWriter *writer;
    WyreboxEmlIngestor *ingestor;
    WyreboxDaemonDeliveryMaterialization *materialization;
} Fixture;

static void
remove_tree (const char *path)
{
    g_autoptr (GDir) dir = g_dir_open (path, 0, NULL);
    const char *name = NULL;

    if (dir == NULL) {
        (void)g_remove (path);
        return;
    }

    while ((name = g_dir_read_name (dir)) != NULL) {
        g_autofree char *child = g_build_filename (path, name, NULL);

        remove_tree (child);
    }

    (void)g_rmdir (path);
}

static void
fixture_set_up (Fixture *fixture, gconstpointer user_data)
{
    g_autoptr (GError) error = NULL;
    g_autofree gchar *object_root = NULL;

    fixture->root = g_dir_make_tmp ("wyrebox-daemon-fact-extraction-XXXXXX",
            &error);
    g_assert_no_error (error);
    object_root = g_build_filename (fixture->root, "objects", NULL);
    fixture->journal_root = g_build_filename (fixture->root, "journal", NULL);
    fixture->catalog_path = g_build_filename (fixture->root, "catalog.duckdb",
            NULL);
    fixture->rules_path = g_build_filename (fixture->root, "extraction.rules",
            NULL);
    g_assert_true (g_file_set_contents (fixture->rules_path,
        "[dictionary:crlf]\nfield=subject\nmatch=crlf\nproject=crlf-project\n",
        -1, &error));
    g_assert_no_error (error);

    fixture->object_store = wyrebox_local_object_store_new (object_root,
            &error);
    g_assert_no_error (error);
    fixture->writer = wyrebox_journal_writer_new (fixture->journal_root,
            &error);
    g_assert_no_error (error);
    fixture->ingestor = wyrebox_eml_ingestor_new_with_journal (
        fixture->object_store, fixture->writer);
    g_assert_true (wyrebox_daemon_runtime_prepare_catalog (
            fixture->journal_root, fixture->catalog_path, FALSE, &error));
    g_assert_no_error (error);
    fixture->materialization = wyrebox_daemon_delivery_materialization_new (
        fixture->catalog_path, fixture->journal_root, fixture->writer,
        fixture->object_store, &error);
    g_assert_no_error (error);
}

static void
fixture_tear_down (Fixture *fixture, gconstpointer user_data)
{
    wyrebox_daemon_delivery_materialization_stop (fixture->materialization);
    g_clear_object (&fixture->materialization);
    g_clear_object (&fixture->ingestor);
    g_clear_object (&fixture->writer);
    g_clear_object (&fixture->object_store);
    remove_tree (fixture->root);
    g_clear_pointer (&fixture->rules_path, g_free);
    g_clear_pointer (&fixture->catalog_path, g_free);
    g_clear_pointer (&fixture->journal_root, g_free);
    g_clear_pointer (&fixture->root, g_free);
}

static void
ingest (Fixture *fixture, const gchar *fixture_name, const gchar *delivery_id,
    const gchar *account_id)
{
    const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
    const gchar *const recipients[] = { "user@example.test", NULL };
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    g_autoptr (GBytes) input = NULL;
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxEmlIngestResult) result = { 0 };
    gsize length = 0;

    g_assert_nonnull (fixture_dir);
    path = g_build_filename (fixture_dir, fixture_name, NULL);
    g_assert_true (g_file_get_contents (path, &contents, &length, &error));
    g_assert_no_error (error);
    input = g_bytes_new_take (g_steal_pointer (&contents), length);
    g_assert_true (wyrebox_eml_ingestor_ingest_delivery_bytes (
            fixture->ingestor, input, delivery_id, NULL, account_id,
            "sender@example.test", recipients, &result, &error));
    g_assert_no_error (error);
}

static void
catch_up (Fixture *fixture)
{
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_delivery_materialization_catch_up (
            fixture->materialization, &error));
    g_assert_no_error (error);
}

static gchar *
query_catalog_string (const gchar *catalog_path, const gchar *sql)
{
    duckdb_database database = NULL;
    duckdb_connection connection = NULL;
    duckdb_result result;
    g_autoptr (GError) error = NULL;
    char *value = NULL;
    gchar *copy = NULL;

    g_assert_true (wyrebox_duckdb_open_shared (catalog_path, &database,
        &error));
    g_assert_no_error (error);
    g_assert_cmpint (duckdb_connect (database, &connection), ==,
        DuckDBSuccess);
    if (duckdb_query (connection, sql, &result) != DuckDBSuccess)
        g_error ("query failed: %s: %s", sql, duckdb_result_error (&result));
    value = duckdb_value_varchar (&result, 0, 0);
    copy = g_strdup (value != NULL ? value : "");
    duckdb_free (value);
    duckdb_destroy_result (&result);
    duckdb_disconnect (&connection);
    duckdb_close (&database);

    return copy;
}

static WyreboxDaemonFactExtraction *
new_extraction (Fixture *fixture)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxFactExtractionRules) rules = NULL;
    WyreboxDaemonFactExtraction *extraction = NULL;

    rules = wyrebox_fact_extraction_rules_new_from_file (fixture->rules_path,
            &error);
    g_assert_no_error (error);
    extraction = wyrebox_daemon_fact_extraction_new (fixture->catalog_path,
            rules, fixture->object_store, fixture->writer, &error);
    g_assert_no_error (error);
    g_assert_nonnull (extraction);

    return extraction;
}

static void
test_extracts_each_materialized_message_once (Fixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (WyreboxDaemonFactExtraction) extraction = NULL;
    g_autoptr (GError) error = NULL;
    g_autofree gchar *markers = NULL;
    g_autofree gchar *facts = NULL;
    guint appended = 0;

    ingest (fixture, "simple-crlf.eml", "delivery-a1", "account-a");
    ingest (fixture, "html-message.eml", "delivery-a2", "account-a");
    ingest (fixture, "duplicate-message-id.eml", "delivery-b1", "account-b");
    catch_up (fixture);
    extraction = new_extraction (fixture);

    g_assert_true (wyrebox_daemon_fact_extraction_extract_account (extraction,
        "account-a", &appended, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (appended, ==, 2);
    catch_up (fixture);

    markers = query_catalog_string (fixture->catalog_path,
            "SELECT string_agg(e.account_id || ' ' || e.fact_count, ', ' "
            "ORDER BY m.journal_sequence) FROM message_fact_extractions e "
            "JOIN messages m ON m.message_id = e.message_id;");
    g_assert_cmpstr (markers, ==, "account-a 8, account-a 7");
    facts = query_catalog_string (fixture->catalog_path,
            "SELECT string_agg(predicate || ' ' || source || ' ' || "
            "json_extract_string(args_json, '$[1]'), ', ' "
            "ORDER BY predicate, json_extract_string(args_json, '$[1]')) "
            "FROM message_facts "
            "WHERE predicate IN ('message_id', 'project_keyword');");
    g_assert_cmpstr (facts, ==,
        "message_id header:message-id <html-message@example.test>, "
        "message_id header:message-id <simple-crlf@example.test>, "
        "project_keyword dictionary:subject:crlf crlf-project");

    g_assert_true (wyrebox_daemon_fact_extraction_extract_account (extraction,
        "account-a", &appended, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (appended, ==, 0);
}

static void
test_unreadable_object_fails_without_appending (Fixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (WyreboxDaemonFactExtraction) extraction = NULL;
    g_autoptr (GError) error = NULL;
    guint64 end_before = 0;
    guint appended = 7;

    catch_up (fixture);
    {
        duckdb_database database = NULL;
        duckdb_connection connection = NULL;
        duckdb_result result;

        g_assert_true (wyrebox_duckdb_open_shared (fixture->catalog_path,
            &database, &error));
        g_assert_no_error (error);
        g_assert_cmpint (duckdb_connect (database, &connection), ==,
            DuckDBSuccess);
        g_assert_cmpint (duckdb_query (connection,
            "INSERT INTO messages (message_id, account_id, object_id, "
            "journal_offset, journal_sequence) VALUES ('journal:9:9', "
            "'account-c', 'sha256:"
            "0000000000000000000000000000000000000000000000000000000000000000"
            "', 9, 9);", &result), ==, DuckDBSuccess);
        duckdb_destroy_result (&result);
        duckdb_disconnect (&connection);
        duckdb_close (&database);
    }
    extraction = new_extraction (fixture);
    end_before = wyrebox_journal_writer_get_durable_end (fixture->writer);

    g_assert_false (wyrebox_daemon_fact_extraction_extract_account (extraction,
        "account-c", &appended, &error));
    g_assert_nonnull (error);
    g_assert_nonnull (g_strstr_len (error->message, -1, "journal:9:9"));
    g_assert_cmpuint (appended, ==, 0);
    g_assert_cmpuint (wyrebox_journal_writer_get_durable_end (fixture->writer),
        ==, end_before);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add ("/daemon/fact-extraction/extracts-each-message-once",
        Fixture, NULL, fixture_set_up,
        test_extracts_each_materialized_message_once, fixture_tear_down);
    g_test_add ("/daemon/fact-extraction/unreadable-object-fails",
        Fixture, NULL, fixture_set_up,
        test_unreadable_object_fails_without_appending, fixture_tear_down);

    return g_test_run ();
}
