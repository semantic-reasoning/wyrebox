#include "wyrebox-daemon-delivery-materialization.h"
#include "wyrebox-daemon-mailbox-catalog-duckdb.h"
#include "wyrebox-daemon-runtime.h"
#include "wyrebox-duckdb-shared.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-schema-metadata-store.h"

#include <duckdb.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

#define JOURNAL_SEGMENT_NAME "00000000000000000000.wbj"

typedef struct
{
    gchar *root;
    gchar *object_root;
    gchar *journal_root;
    gchar *catalog_path;
    WyreboxLocalObjectStore *object_store;
    WyreboxJournalWriter *writer;
    WyreboxEmlIngestor *ingestor;
} Fixture;

static void
remove_tree (const char *path)
{
    g_autoptr (GDir) dir = NULL;
    const char *name = NULL;

    dir = g_dir_open (path, 0, NULL);
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

static gchar *
prepare_catalog (const Fixture *fixture, const gchar *name)
{
    g_autoptr (GError) error = NULL;
    gchar *path = g_build_filename (fixture->root, name, NULL);

    g_assert_true (wyrebox_daemon_runtime_prepare_catalog (
            fixture->journal_root, path, FALSE, &error));
    g_assert_no_error (error);

    return path;
}

static void
fixture_set_up (Fixture *fixture)
{
    g_autoptr (GError) error = NULL;

    fixture->root = g_dir_make_tmp ("wyrebox-daemon-materialization-XXXXXX",
            &error);
    g_assert_no_error (error);
    fixture->object_root = g_build_filename (fixture->root, "objects", NULL);
    fixture->journal_root = g_build_filename (fixture->root, "journal", NULL);

    fixture->object_store =
        wyrebox_local_object_store_new (fixture->object_root, &error);
    g_assert_no_error (error);
    fixture->writer = wyrebox_journal_writer_new (fixture->journal_root,
            &error);
    g_assert_no_error (error);
    fixture->ingestor = wyrebox_eml_ingestor_new_with_journal (
        fixture->object_store, fixture->writer);
    fixture->catalog_path = prepare_catalog (fixture, "catalog.duckdb");
}

static void
fixture_tear_down (Fixture *fixture)
{
    g_clear_object (&fixture->ingestor);
    g_clear_object (&fixture->writer);
    g_clear_object (&fixture->object_store);
    remove_tree (fixture->root);
    g_clear_pointer (&fixture->catalog_path, g_free);
    g_clear_pointer (&fixture->journal_root, g_free);
    g_clear_pointer (&fixture->object_root, g_free);
    g_clear_pointer (&fixture->root, g_free);
}

static WyreboxDaemonDeliveryMaterialization *
new_materialization (const Fixture *fixture, const gchar *catalog_path)
{
    g_autoptr (GError) error = NULL;
    WyreboxDaemonDeliveryMaterialization *materialization = NULL;

    materialization = wyrebox_daemon_delivery_materialization_new (
        catalog_path, fixture->journal_root, fixture->writer,
        fixture->object_store, &error);
    g_assert_no_error (error);
    g_assert_nonnull (materialization);

    return materialization;
}

static void
ingest (Fixture *fixture, const gchar *fixture_name, const gchar *delivery_id,
    const gchar *account_id, WyreboxEmlIngestResult *out_result)
{
    const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
    const gchar *const recipients[] = { "user@example.test", NULL };
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    g_autoptr (GBytes) input = NULL;
    g_autoptr (GError) error = NULL;
    gsize length = 0;

    g_assert_nonnull (fixture_dir);
    path = g_build_filename (fixture_dir, fixture_name, NULL);
    g_assert_true (g_file_get_contents (path, &contents, &length, &error));
    g_assert_no_error (error);
    input = g_bytes_new_take (g_steal_pointer (&contents), length);

    if (account_id == NULL) {
        g_assert_true (wyrebox_eml_ingestor_ingest_bytes (fixture->ingestor,
            input, out_result, &error));
    } else {
        g_assert_true (wyrebox_eml_ingestor_ingest_delivery_bytes (
                fixture->ingestor, input, delivery_id, NULL, account_id,
                "sender@example.test", recipients, out_result, &error));
    }
    g_assert_no_error (error);
}

static void
exec_catalog_sql (const gchar *catalog_path, const gchar *sql)
{
    duckdb_database database = NULL;
    duckdb_connection connection = NULL;
    duckdb_result result;
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_duckdb_open_shared (catalog_path, &database,
        &error));
    g_assert_no_error (error);
    g_assert_cmpint (duckdb_connect (database, &connection), ==,
        DuckDBSuccess);
    if (duckdb_query (connection, sql, &result) != DuckDBSuccess)
        g_error ("query failed: %s: %s", sql, duckdb_result_error (&result));
    duckdb_destroy_result (&result);
    duckdb_disconnect (&connection);
    duckdb_close (&database);
}

static guint64
query_catalog_uint64 (const gchar *catalog_path, const gchar *sql)
{
    duckdb_database database = NULL;
    duckdb_connection connection = NULL;
    duckdb_result result;
    g_autoptr (GError) error = NULL;
    guint64 value = 0;

    g_assert_true (wyrebox_duckdb_open_shared (catalog_path, &database,
        &error));
    g_assert_no_error (error);
    g_assert_cmpint (duckdb_connect (database, &connection), ==,
        DuckDBSuccess);
    if (duckdb_query (connection, sql, &result) != DuckDBSuccess)
        g_error ("query failed: %s: %s", sql, duckdb_result_error (&result));
    g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
    value = duckdb_value_uint64 (&result, 0, 0);
    duckdb_destroy_result (&result);
    duckdb_disconnect (&connection);
    duckdb_close (&database);

    return value;
}

static guint64
inbox_uid (const gchar *catalog_path, const gchar *account_id,
    const WyreboxEmlIngestResult *result)
{
    g_autofree gchar *sql = g_strdup_printf ("SELECT m.uid FROM "
            "mailbox_memberships m JOIN mailboxes b "
            "ON b.mailbox_id = m.mailbox_id WHERE b.account_id = '%s' "
            "AND b.imap_name = 'INBOX' AND m.journal_offset = %"
            G_GUINT64_FORMAT " AND m.journal_sequence = %" G_GUINT64_FORMAT ";",
            account_id, result->journal_offset, result->journal_sequence);

    return query_catalog_uint64 (catalog_path, sql);
}

static void
assert_checkpoint (const gchar *catalog_path,
    const WyreboxEmlIngestResult *result)
{
    g_assert_cmpuint (query_catalog_uint64 (catalog_path,
        "SELECT journal_offset FROM materialization_checkpoint "
        "WHERE checkpoint_key = 'materialization';"), ==,
        result->journal_offset);
    g_assert_cmpuint (query_catalog_uint64 (catalog_path,
        "SELECT journal_sequence FROM materialization_checkpoint "
        "WHERE checkpoint_key = 'materialization';"), ==,
        result->journal_sequence);
}

static void
test_catch_up_materializes_into_account_inbox (Fixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (WyreboxDaemonDeliveryMaterialization) materialization = NULL;
    g_autoptr (WyreboxDaemonMailboxCatalogDuckDB) catalog = NULL;
    g_auto (WyreboxDaemonMailboxSelectRequest) request = { 0 };
    g_auto (WyreboxDaemonMailboxSelectResult) result = { 0 };
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_auto (WyreboxEmlIngestResult) second = { 0 };
    g_autoptr (GError) error = NULL;

    catalog = wyrebox_daemon_mailbox_catalog_duckdb_new (
        fixture->catalog_path, &error);
    g_assert_no_error (error);
    materialization = new_materialization (fixture, fixture->catalog_path);

    ingest (fixture, "simple-crlf.eml", "delivery-1", "account-1", &first);
    ingest (fixture, "duplicate-message-id.eml", "delivery-2", "account-1",
        &second);
    g_assert_true (wyrebox_daemon_delivery_materialization_catch_up (
            materialization, &error));
    g_assert_no_error (error);

    g_assert_true (wyrebox_daemon_mailbox_select_request_init (&request,
        "account-1", NULL, "INBOX", &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_mailbox_catalog_duckdb_select (NULL,
        &request, &result, catalog, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (result.message_count, ==, 2);
    g_assert_cmpuint (result.uid_next, ==, 3);
    g_assert_cmpuint (inbox_uid (fixture->catalog_path, "account-1", &second),
        ==, 2);
}

static void
test_catch_up_ignores_bytes_past_durable_end (Fixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (WyreboxDaemonDeliveryMaterialization) materialization = NULL;
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_autoptr (GError) error = NULL;
    g_autofree gchar *segment_path = NULL;
    int fd = -1;

    materialization = new_materialization (fixture, fixture->catalog_path);
    ingest (fixture, "simple-crlf.eml", "delivery-1", "account-1", &first);

    segment_path = g_build_filename (fixture->journal_root,
            JOURNAL_SEGMENT_NAME, NULL);
    fd = open (segment_path, O_WRONLY | O_APPEND | O_CLOEXEC);
    g_assert_cmpint (fd, >=, 0);
    g_assert_cmpint (write (fd, "WYREJNL1", 8), ==, 8);
    g_assert_cmpint (close (fd), ==, 0);

    g_assert_true (wyrebox_daemon_delivery_materialization_catch_up (
            materialization, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (inbox_uid (fixture->catalog_path, "account-1", &first),
        ==, 1);
    assert_checkpoint (fixture->catalog_path, &first);
}

static void
test_retry_resumes_after_failed_run (Fixture *fixture, gconstpointer user_data)
{
    g_autoptr (WyreboxDaemonDeliveryMaterialization) materialization = NULL;
    g_autoptr (WyreboxDaemonDeliveryMaterialization) rebuild = NULL;
    g_autofree gchar *rebuilt_path = NULL;
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_auto (WyreboxEmlIngestResult) second = { 0 };
    g_auto (WyreboxEmlIngestResult) third = { 0 };
    g_autoptr (GError) error = NULL;
    gint64 deadline = 0;

    exec_catalog_sql (fixture->catalog_path,
        "INSERT INTO accounts (account_id) VALUES ('account-b');");
    exec_catalog_sql (fixture->catalog_path,
        "INSERT INTO mailboxes ("
        "mailbox_id, account_id, imap_name, is_selectable, is_visible"
        ") VALUES ('inbox-b', 'account-b', 'INBOX', FALSE, TRUE);");

    materialization = new_materialization (fixture, fixture->catalog_path);
    wyrebox_daemon_delivery_materialization_set_retry_interval (
        materialization, 10);
    ingest (fixture, "simple-crlf.eml", "delivery-1", "account-a", &first);
    ingest (fixture, "duplicate-message-id.eml", "delivery-2", "account-b",
        &second);
    ingest (fixture, "simple-crlf.eml", "delivery-3", "account-a", &third);

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
        "*delivery materialization failed*inbox-b*");
    wyrebox_daemon_delivery_materialization_catch_up_or_schedule_retry (
        materialization);
    g_test_assert_expected_messages ();
    g_assert_true (wyrebox_daemon_delivery_materialization_is_retry_pending (
            materialization));
    g_assert_cmpuint (inbox_uid (fixture->catalog_path, "account-a", &first),
        ==, 1);
    assert_checkpoint (fixture->catalog_path, &first);

    exec_catalog_sql (fixture->catalog_path,
        "UPDATE mailboxes SET is_selectable = TRUE "
        "WHERE mailbox_id = 'inbox-b';");

    deadline = g_get_monotonic_time () + 10 * G_TIME_SPAN_SECOND;
    while (wyrebox_daemon_delivery_materialization_is_retry_pending (
            materialization)) {
        g_assert_cmpint (g_get_monotonic_time (), <, deadline);
        g_main_context_iteration (NULL, TRUE);
    }

    g_assert_cmpuint (inbox_uid (fixture->catalog_path, "account-b", &second),
        ==, 1);
    g_assert_cmpuint (inbox_uid (fixture->catalog_path, "account-a", &third),
        ==, 2);
    assert_checkpoint (fixture->catalog_path, &third);

    rebuilt_path = prepare_catalog (fixture, "rebuilt.duckdb");
    rebuild = new_materialization (fixture, rebuilt_path);
    g_assert_true (wyrebox_daemon_delivery_materialization_catch_up (rebuild,
        &error));
    g_assert_no_error (error);
    g_assert_cmpuint (inbox_uid (rebuilt_path, "account-a", &first), ==, 1);
    g_assert_cmpuint (inbox_uid (rebuilt_path, "account-b", &second), ==, 1);
    g_assert_cmpuint (inbox_uid (rebuilt_path, "account-a", &third), ==, 2);
}

static void
test_records_before_checkpoint_need_no_account (Fixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (WyreboxDaemonDeliveryMaterialization) materialization = NULL;
    g_autoptr (WyreboxSchemaMetadataStore) store = NULL;
    g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };
    g_auto (WyreboxEmlIngestResult) legacy = { 0 };
    g_auto (WyreboxEmlIngestResult) delivered = { 0 };
    g_autoptr (GError) error = NULL;

    ingest (fixture, "simple-crlf.eml", NULL, NULL, &legacy);

    store = wyrebox_schema_metadata_store_new_duckdb (fixture->catalog_path,
            &error);
    g_assert_no_error (error);
    g_assert_true (wyrebox_schema_metadata_store_load (store, &metadata,
        &error));
    g_assert_no_error (error);
    metadata.materialization_checkpoint_present = TRUE;
    metadata.materialization_checkpoint_journal_offset = legacy.journal_offset;
    metadata.materialization_checkpoint_sequence = legacy.journal_sequence;
    g_assert_true (wyrebox_schema_metadata_store_save (store, &metadata,
        &error));
    g_assert_no_error (error);
    g_clear_object (&store);

    ingest (fixture, "duplicate-message-id.eml", "delivery-2", "account-1",
        &delivered);
    materialization = new_materialization (fixture, fixture->catalog_path);
    g_assert_true (wyrebox_daemon_delivery_materialization_catch_up (
            materialization, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (inbox_uid (fixture->catalog_path, "account-1",
        &delivered), ==, 1);
}

static void
test_pending_record_without_account_fails (Fixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (WyreboxDaemonDeliveryMaterialization) materialization = NULL;
    g_auto (WyreboxEmlIngestResult) legacy = { 0 };
    g_autoptr (GError) error = NULL;

    ingest (fixture, "simple-crlf.eml", NULL, NULL, &legacy);
    materialization = new_materialization (fixture, fixture->catalog_path);

    g_assert_false (wyrebox_daemon_delivery_materialization_catch_up (
            materialization, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_false (wyrebox_daemon_delivery_materialization_is_retry_pending (
            materialization));
}

static void
test_stop_cancels_retry_and_releases_reference (Fixture *fixture,
    gconstpointer user_data)
{
    WyreboxDaemonDeliveryMaterialization *materialization = NULL;
    g_auto (WyreboxEmlIngestResult) delivered = { 0 };

    exec_catalog_sql (fixture->catalog_path,
        "INSERT INTO accounts (account_id) VALUES ('account-b');");
    exec_catalog_sql (fixture->catalog_path,
        "INSERT INTO mailboxes ("
        "mailbox_id, account_id, imap_name, is_selectable, is_visible"
        ") VALUES ('inbox-b', 'account-b', 'INBOX', FALSE, TRUE);");
    ingest (fixture, "simple-crlf.eml", "delivery-1", "account-b", &delivered);

    materialization = new_materialization (fixture, fixture->catalog_path);
    g_object_add_weak_pointer (G_OBJECT (materialization),
        (gpointer *)&materialization);

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
        "*delivery materialization failed*inbox-b*");
    wyrebox_daemon_delivery_materialization_catch_up_or_schedule_retry (
        materialization);
    g_test_assert_expected_messages ();
    g_assert_true (wyrebox_daemon_delivery_materialization_is_retry_pending (
            materialization));

    wyrebox_daemon_delivery_materialization_stop (materialization);
    g_assert_false (wyrebox_daemon_delivery_materialization_is_retry_pending (
            materialization));

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
        "*delivery materialization failed*inbox-b*");
    wyrebox_daemon_delivery_materialization_catch_up_or_schedule_retry (
        materialization);
    g_test_assert_expected_messages ();
    g_assert_false (wyrebox_daemon_delivery_materialization_is_retry_pending (
            materialization));

    g_object_unref (materialization);
    g_assert_null (materialization);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add ("/daemon/delivery-materialization/catch-up-into-account-inbox",
        Fixture, NULL, (void (*)(Fixture *, gconstpointer)) fixture_set_up,
        test_catch_up_materializes_into_account_inbox,
        (void (*)(Fixture *, gconstpointer)) fixture_tear_down);
    g_test_add ("/daemon/delivery-materialization/ignores-past-durable-end",
        Fixture, NULL, (void (*)(Fixture *, gconstpointer)) fixture_set_up,
        test_catch_up_ignores_bytes_past_durable_end,
        (void (*)(Fixture *, gconstpointer)) fixture_tear_down);
    g_test_add ("/daemon/delivery-materialization/retry-after-failed-run",
        Fixture, NULL, (void (*)(Fixture *, gconstpointer)) fixture_set_up,
        test_retry_resumes_after_failed_run,
        (void (*)(Fixture *, gconstpointer)) fixture_tear_down);
    g_test_add ("/daemon/delivery-materialization/"
        "pre-checkpoint-records-need-no-account",
        Fixture, NULL, (void (*)(Fixture *, gconstpointer)) fixture_set_up,
        test_records_before_checkpoint_need_no_account,
        (void (*)(Fixture *, gconstpointer)) fixture_tear_down);
    g_test_add ("/daemon/delivery-materialization/"
        "pending-record-without-account-fails",
        Fixture, NULL, (void (*)(Fixture *, gconstpointer)) fixture_set_up,
        test_pending_record_without_account_fails,
        (void (*)(Fixture *, gconstpointer)) fixture_tear_down);
    g_test_add ("/daemon/delivery-materialization/"
        "stop-cancels-retry-and-releases-reference",
        Fixture, NULL, (void (*)(Fixture *, gconstpointer)) fixture_set_up,
        test_stop_cancels_retry_and_releases_reference,
        (void (*)(Fixture *, gconstpointer)) fixture_tear_down);

    return g_test_run ();
}
