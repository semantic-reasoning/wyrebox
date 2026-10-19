#include "wyrebox-daemon-delivery-materialization.h"
#include "wyrebox-daemon-memberships-expunge-dispatcher.h"
#include "wyrebox-daemon-memberships-expunge-journal.h"
#include "wyrebox-daemon-runtime.h"
#include "wyrebox-duckdb-shared.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

typedef struct
{
    gchar *root;
    gchar *object_root;
    gchar *journal_root;
    gchar *catalog_path;
    WyreboxLocalObjectStore *object_store;
    WyreboxJournalWriter *writer;
    WyreboxEmlIngestor *ingestor;
    WyreboxDaemonDeliveryMaterialization *materialization;
    WyreboxDaemonMembershipsExpungeService *service;
    WyreboxEmlIngestResult first;
    WyreboxEmlIngestResult second;
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

static void
ingest (Fixture *fixture, const gchar *fixture_name, const gchar *delivery_id,
    WyreboxEmlIngestResult *out_result)
{
    const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
    const gchar *const recipients[] = { "user@example.test", NULL };
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    g_autoptr (GBytes) input = NULL;
    g_autoptr (GError) error = NULL;
    gsize length = 0;

    path = g_build_filename (fixture_dir, fixture_name, NULL);
    g_assert_true (g_file_get_contents (path, &contents, &length, &error));
    input = g_bytes_new_take (g_steal_pointer (&contents), length);
    g_assert_true (wyrebox_eml_ingestor_ingest_delivery_bytes (
            fixture->ingestor, input, delivery_id, NULL, "account-1",
            "sender@example.test", recipients, out_result, &error));
    g_assert_no_error (error);
}

static void
fixture_set_up (Fixture *fixture, gconstpointer user_data)
{
    g_autoptr (GError) error = NULL;

    fixture->root = g_dir_make_tmp ("wyrebox-daemon-expunge-XXXXXX", &error);
    g_assert_no_error (error);
    fixture->object_root = g_build_filename (fixture->root, "objects", NULL);
    fixture->journal_root = g_build_filename (fixture->root, "journal", NULL);
    fixture->object_store =
        wyrebox_local_object_store_new (fixture->object_root, &error);
    fixture->writer = wyrebox_journal_writer_new (fixture->journal_root,
            &error);
    fixture->ingestor = wyrebox_eml_ingestor_new_with_journal (
        fixture->object_store, fixture->writer);

    ingest (fixture, "simple-crlf.eml", "delivery-1", &fixture->first);
    ingest (fixture, "html-message.eml", "delivery-2", &fixture->second);

    fixture->catalog_path = g_build_filename (fixture->root, "catalog.duckdb",
            NULL);
    g_assert_true (wyrebox_daemon_runtime_prepare_catalog (
            fixture->journal_root, fixture->catalog_path, FALSE, &error));
    fixture->materialization = wyrebox_daemon_delivery_materialization_new (
        fixture->catalog_path, fixture->journal_root, fixture->writer,
        fixture->object_store, &error);
    g_assert_true (wyrebox_daemon_delivery_materialization_catch_up
            (fixture->materialization, &error));
    fixture->service =
        wyrebox_daemon_memberships_expunge_service_new_journaled
            (fixture->catalog_path, fixture->writer, fixture->materialization,
            &error);
    g_assert_no_error (error);
}

static void
fixture_tear_down (Fixture *fixture, gconstpointer user_data)
{
    g_clear_object (&fixture->service);
    if (fixture->materialization != NULL)
        wyrebox_daemon_delivery_materialization_stop (fixture->materialization);
    g_clear_object (&fixture->materialization);
    wyrebox_eml_ingest_result_clear (&fixture->first);
    wyrebox_eml_ingest_result_clear (&fixture->second);
    g_clear_object (&fixture->ingestor);
    g_clear_object (&fixture->writer);
    g_clear_object (&fixture->object_store);
    remove_tree (fixture->root);
    g_clear_pointer (&fixture->catalog_path, g_free);
    g_clear_pointer (&fixture->journal_root, g_free);
    g_clear_pointer (&fixture->object_root, g_free);
    g_clear_pointer (&fixture->root, g_free);
}

static void
dispatch_expunge (const Fixture *fixture, guint64 uid_validity,
    const guint64 *uids, gsize n_uids, WyreboxDaemonResponseFrame *out_frame)
{
    g_auto (WyreboxDaemonMembershipsExpungeRequest) request = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_memberships_expunge_request_init (&request,
        "account-1", "inbox:account-1", uid_validity, uids, n_uids, &error));
    g_assert_true (wyrebox_daemon_memberships_expunge_dispatch (
            fixture->service, "request-expunge", "dovecot", "account-1",
            "dovecot-storage", "imap-1", &request, out_frame, &error));
    g_assert_no_error (error);
}

static guint64
query_uint64 (const gchar *catalog_path, const gchar *sql)
{
    duckdb_database database = NULL;
    duckdb_connection connection = NULL;
    duckdb_result result;
    g_autoptr (GError) error = NULL;
    char *value = NULL;
    guint64 out = 0;

    g_assert_true (wyrebox_duckdb_open_shared (catalog_path, &database,
        &error));
    g_assert_cmpint (duckdb_connect (database, &connection), ==,
        DuckDBSuccess);
    g_assert_cmpint (duckdb_query (connection, sql, &result), ==,
        DuckDBSuccess);
    value = duckdb_value_varchar (&result, 0, 0);
    out = g_ascii_strtoull (value, NULL, 10);
    duckdb_free (value);
    duckdb_destroy_result (&result);
    duckdb_disconnect (&connection);
    duckdb_close (&database);
    return out;
}

static void
test_expunge_tombs_visible_uids (Fixture *fixture, gconstpointer user_data)
{
    guint64 uids[] = { 1, 2 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    guint64 durable_end = 0;

    durable_end = wyrebox_journal_writer_get_durable_end (fixture->writer);
    dispatch_expunge (fixture, 1, uids, G_N_ELEMENTS (uids), &frame);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
    g_assert_cmpuint (frame.success.journal_sequence, >, 0);
    g_assert_cmpuint (wyrebox_journal_writer_get_durable_end (fixture->writer),
        >, durable_end);
    g_assert_cmpuint (query_uint64 (fixture->catalog_path,
        "SELECT CAST(COUNT(*) AS UBIGINT) FROM mailbox_memberships "
        "WHERE mailbox_id = 'inbox:account-1' AND is_visible = TRUE;"), ==, 0);
}

static void
test_expunge_noop_when_uids_already_absent (Fixture *fixture,
    gconstpointer user_data)
{
    guint64 uids[] = { 99 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    guint64 durable_end = 0;

    durable_end = wyrebox_journal_writer_get_durable_end (fixture->writer);
    dispatch_expunge (fixture, 1, uids, G_N_ELEMENTS (uids), &frame);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
    g_assert_cmpstr (frame.success.durable_marker, ==, "no-op");
    g_assert_cmpuint (wyrebox_journal_writer_get_durable_end (fixture->writer),
        ==, durable_end);
}

static void
test_stale_uidvalidity_is_conflict (Fixture *fixture, gconstpointer user_data)
{
    guint64 uids[] = { 1 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

    dispatch_expunge (fixture, 2, uids, G_N_ELEMENTS (uids), &frame);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
    g_assert_cmpint (frame.error.error_class, ==,
        WYREBOX_DAEMON_ERROR_CONFLICT);
}

static void
test_unknown_mailbox_is_not_found (Fixture *fixture, gconstpointer user_data)
{
    guint64 uids[] = { 1 };
    g_auto (WyreboxDaemonMembershipsExpungeRequest) request = { 0 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_memberships_expunge_request_init (&request,
        "account-1", "missing-mailbox", 1, uids, G_N_ELEMENTS (uids), &error));
    g_assert_true (wyrebox_daemon_memberships_expunge_dispatch (
            fixture->service, "request-expunge", "dovecot", "account-1",
            "dovecot-storage", "imap-1", &request, &frame, &error));
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
    g_assert_cmpint (frame.error.error_class, ==,
        WYREBOX_DAEMON_ERROR_NOT_FOUND);
}

#define ADD_TEST(path, func) \
        g_test_add ("/daemon-api/memberships-expunge-journal/" path, Fixture, \
            NULL, fixture_set_up, func, fixture_tear_down)

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    ADD_TEST ("tombs-visible-uids", test_expunge_tombs_visible_uids);
    ADD_TEST ("noop-absent-uids", test_expunge_noop_when_uids_already_absent);
    ADD_TEST ("stale-uidvalidity", test_stale_uidvalidity_is_conflict);
    ADD_TEST ("unknown-mailbox", test_unknown_mailbox_is_not_found);

    return g_test_run ();
}
