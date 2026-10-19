#include "wyrebox-daemon-delivery-materialization.h"
#include "wyrebox-daemon-memberships-copy-move-dispatcher.h"
#include "wyrebox-daemon-memberships-copy-move-journal.h"
#include "wyrebox-daemon-runtime.h"
#include "wyrebox-duckdb-shared.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-memberships-copied-payload.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

typedef struct
{
    gchar *root;
    gchar *catalog_path;
    WyreboxJournalWriter *writer;
    WyreboxEmlIngestor *ingestor;
    WyreboxDaemonDeliveryMaterialization *materialization;
    WyreboxDaemonMembershipsCopyMoveService *service;
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
ingest_one (Fixture *fixture, const gchar *delivery_id)
{
    const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
    const gchar *const recipients[] = { "user@example.test", NULL };
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    g_autoptr (GBytes) input = NULL;
    g_autoptr (GError) error = NULL;
    WyreboxEmlIngestResult result = { 0 };
    gsize length = 0;

    path = g_build_filename (fixture_dir, "simple-crlf.eml", NULL);
    g_assert_true (g_file_get_contents (path, &contents, &length, &error));
    input = g_bytes_new_take (g_steal_pointer (&contents), length);
    g_assert_true (wyrebox_eml_ingestor_ingest_delivery_bytes (
            fixture->ingestor, input, delivery_id, NULL, "account-1",
            "sender@example.test", recipients, &result, &error));
    wyrebox_eml_ingest_result_clear (&result);
}

static void
fixture_set_up (Fixture *fixture, gconstpointer user_data)
{
    g_autoptr (GError) error = NULL;
    g_autofree gchar *object_root = NULL;
    g_autofree gchar *journal_root = NULL;
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;

    fixture->root = g_dir_make_tmp ("wyrebox-daemon-copy-move-XXXXXX", &error);
    g_assert_no_error (error);
    object_root = g_build_filename (fixture->root, "objects", NULL);
    journal_root = g_build_filename (fixture->root, "journal", NULL);
    object_store = wyrebox_local_object_store_new (object_root, &error);
    fixture->writer = wyrebox_journal_writer_new (journal_root, &error);
    fixture->ingestor = wyrebox_eml_ingestor_new_with_journal (object_store,
            fixture->writer);

    ingest_one (fixture, "delivery-1");
    ingest_one (fixture, "delivery-2");

    fixture->catalog_path = g_build_filename (fixture->root, "catalog.duckdb",
            NULL);
    g_assert_true (wyrebox_daemon_runtime_prepare_catalog (journal_root,
        fixture->catalog_path, FALSE, &error));
    fixture->materialization = wyrebox_daemon_delivery_materialization_new (
        fixture->catalog_path, journal_root, fixture->writer, object_store,
        &error);
    g_assert_true (wyrebox_daemon_delivery_materialization_catch_up
            (fixture->materialization, &error));
    fixture->service =
        wyrebox_daemon_memberships_copy_move_service_new_journaled
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
    g_clear_object (&fixture->ingestor);
    g_clear_object (&fixture->writer);
    remove_tree (fixture->root);
    g_clear_pointer (&fixture->catalog_path, g_free);
    g_clear_pointer (&fixture->root, g_free);
}

static void
dispatch_copy (const Fixture *fixture, const guint64 *uids, gsize n_uids,
    WyreboxDaemonResponseFrame *out_frame)
{
    g_auto (WyreboxDaemonMembershipsCopyMoveRequest) request = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_memberships_copy_move_request_init (&request,
        "account-1", "inbox:account-1",
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 1, "inbox:account-1", 1,
        uids, n_uids, &error));
    g_assert_true (wyrebox_daemon_memberships_copy_dispatch (fixture->service,
        "request-copy", "dovecot", "account-1", "dovecot-storage", "imap-1",
        &request, out_frame, &error));
    g_assert_no_error (error);
}

static void
dispatch_move (const Fixture *fixture, const guint64 *uids, gsize n_uids,
    WyreboxDaemonResponseFrame *out_frame)
{
    g_auto (WyreboxDaemonMembershipsCopyMoveRequest) request = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_memberships_copy_move_request_init (&request,
        "account-1", "inbox:account-1",
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 1, "inbox:account-1", 1,
        uids, n_uids, &error));
    g_assert_true (wyrebox_daemon_memberships_move_dispatch (fixture->service,
        "request-move", "dovecot", "account-1", "dovecot-storage", "imap-1",
        &request, out_frame, &error));
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
test_copy_creates_destination_uid (Fixture *fixture, gconstpointer user_data)
{
    guint64 uids[] = { 1 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

    dispatch_copy (fixture, uids, G_N_ELEMENTS (uids), &frame);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
    g_assert (strstr (frame.success.summary, "pairs=1:3") != NULL);
    g_assert_cmpuint (query_uint64 (fixture->catalog_path,
        "SELECT CAST(COUNT(*) AS UBIGINT) FROM mailbox_memberships "
        "WHERE mailbox_id = 'inbox:account-1' AND uid = 3 AND is_visible = "
        "TRUE;"), ==, 1);
}

static void
test_move_tombs_source (Fixture *fixture, gconstpointer user_data)
{
    guint64 uids[] = { 2 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

    dispatch_move (fixture, uids, G_N_ELEMENTS (uids), &frame);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
    g_assert_cmpuint (query_uint64 (fixture->catalog_path,
        "SELECT CAST(COUNT(*) AS UBIGINT) FROM mailbox_memberships "
        "WHERE mailbox_id = 'inbox:account-1' AND uid = 2 AND is_visible = "
        "FALSE;"), ==, 1);
}

static void
test_missing_source_is_not_found (Fixture *fixture, gconstpointer user_data)
{
    guint64 uids[] = { 99 };
    g_auto (WyreboxDaemonMembershipsCopyMoveRequest) request = { 0 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_memberships_copy_move_request_init (&request,
        "account-1", "inbox:account-1",
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 1, "inbox:account-1", 1,
        uids, G_N_ELEMENTS (uids), &error));
    g_assert_true (wyrebox_daemon_memberships_copy_dispatch (fixture->service,
        "request-copy", "dovecot", "account-1", "dovecot-storage", "imap-1",
        &request, &frame, &error));
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
    g_assert_cmpint (frame.error.error_class, ==,
        WYREBOX_DAEMON_ERROR_NOT_FOUND);
}

static void
test_virtual_dest_is_permission_denied (Fixture *fixture,
    gconstpointer user_data)
{
    duckdb_database database = NULL;
    duckdb_connection connection = NULL;
    guint64 uids[] = { 1 };
    g_auto (WyreboxDaemonMembershipsCopyMoveRequest) request = { 0 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_duckdb_open_shared (fixture->catalog_path, &database,
        &error));
    g_assert_cmpint (duckdb_connect (database, &connection), ==,
        DuckDBSuccess);
    g_assert_cmpint (duckdb_query (connection,
        "INSERT INTO derived_views (view_id, account_id, imap_name, "
        "definition_ref, is_selectable, is_visible) VALUES "
        "('view-projects', 'account-1', 'Projects', 'wirelog:projects', "
        "TRUE, TRUE);",
        NULL), ==, DuckDBSuccess);
    duckdb_disconnect (&connection);
    duckdb_close (&database);

    g_assert_true (wyrebox_daemon_memberships_copy_move_request_init (&request,
        "account-1", "inbox:account-1",
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, 1, "view-projects", 1,
        uids, G_N_ELEMENTS (uids), &error));
    g_assert_true (wyrebox_daemon_memberships_copy_dispatch (fixture->service,
        "request-copy", "dovecot", "account-1", "dovecot-storage", "imap-1",
        &request, &frame, &error));
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
    g_assert_cmpint (frame.error.error_class, ==,
        WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

#define ADD_TEST(path, func) \
        g_test_add ("/daemon-api/memberships-copy-move-journal/" path, \
            Fixture, NULL, fixture_set_up, func, fixture_tear_down)

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    ADD_TEST ("copy-creates-dest", test_copy_creates_destination_uid);
    ADD_TEST ("move-tombs-source", test_move_tombs_source);
    ADD_TEST ("missing-source", test_missing_source_is_not_found);
    ADD_TEST ("virtual-dest-denied", test_virtual_dest_is_permission_denied);

    return g_test_run ();
}
