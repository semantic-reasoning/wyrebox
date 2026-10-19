#include "wyrebox-daemon-delivery-materialization.h"
#include "wyrebox-daemon-message-append-dispatcher.h"
#include "wyrebox-daemon-message-append-journal.h"
#include "wyrebox-daemon-runtime.h"
#include "wyrebox-duckdb-shared.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-message-appended-payload.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

typedef struct
{
    gchar *root;
    gchar *catalog_path;
    gchar *object_root;
    WyreboxJournalWriter *writer;
    WyreboxLocalObjectStore *object_store;
    WyreboxDaemonDeliveryMaterialization *materialization;
    WyreboxDaemonMessageAppendService *service;
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

static GBytes *
load_fixture_eml (void)
{
    const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    gsize length = 0;
    g_autoptr (GError) error = NULL;

    path = g_build_filename (fixture_dir, "simple-crlf.eml", NULL);
    g_assert_true (g_file_get_contents (path, &contents, &length, &error));
    return g_bytes_new_take (g_steal_pointer (&contents), length);
}

static void
dispatch_append (const Fixture *fixture, const char *mailbox_id,
    guint64 uid_validity, GBytes *message,
    WyreboxDaemonResponseFrame *out_frame)
{
    g_auto (WyreboxDaemonMessageAppendRequest) request = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_message_append_request_init (&request,
        "account-1", mailbox_id, uid_validity, message, 0, NULL, NULL,
        &error));
    g_assert_true (wyrebox_daemon_message_append_dispatch (fixture->service,
        "request-append", "dovecot", "account-1", "dovecot-storage", "imap-1",
        &request, out_frame, &error));
    g_assert_no_error (error);
}

static void
fixture_set_up (Fixture *fixture, gconstpointer user_data)
{
    g_autoptr (GError) error = NULL;
    g_autofree gchar *journal_root = NULL;

    fixture->root = g_dir_make_tmp ("wyrebox-daemon-append-XXXXXX", &error);
    g_assert_no_error (error);
    fixture->object_root = g_build_filename (fixture->root, "objects", NULL);
    fixture->catalog_path = g_build_filename (fixture->root, "catalog.duckdb",
            NULL);
    journal_root = g_build_filename (fixture->root, "journal", NULL);
    fixture->object_store =
        wyrebox_local_object_store_new (fixture->object_root,
            &error);
    fixture->writer = wyrebox_journal_writer_new (journal_root, &error);

    g_assert_true (wyrebox_daemon_runtime_prepare_catalog (journal_root,
        fixture->catalog_path, FALSE, &error));
    fixture->materialization = wyrebox_daemon_delivery_materialization_new (
        fixture->catalog_path, journal_root, fixture->writer,
        fixture->object_store, &error);
    fixture->service = wyrebox_daemon_message_append_service_new_journaled
            (fixture->catalog_path, fixture->writer, fixture->object_store,
            fixture->materialization, &error);
    g_assert_no_error (error);
}

static void
fixture_tear_down (Fixture *fixture, gconstpointer user_data)
{
    g_clear_object (&fixture->service);
    if (fixture->materialization != NULL)
        wyrebox_daemon_delivery_materialization_stop (fixture->materialization);
    g_clear_object (&fixture->materialization);
    g_clear_object (&fixture->object_store);
    g_clear_object (&fixture->writer);
    remove_tree (fixture->root);
    g_clear_pointer (&fixture->catalog_path, g_free);
    g_clear_pointer (&fixture->object_root, g_free);
    g_clear_pointer (&fixture->root, g_free);
}

static guint64
query_uint64 (const gchar *catalog_path, const gchar *sql)
{
    duckdb_database database = NULL;
    duckdb_connection connection = NULL;
    duckdb_result result;
    char *value = NULL;
    guint64 out = 0;

    g_assert_true (wyrebox_duckdb_open_shared (catalog_path, &database, NULL));
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
test_append_creates_membership_and_object (Fixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (GBytes) message = load_fixture_eml ();
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    g_autoptr (GBytes) stored = NULL;
    g_autoptr (GError) error = NULL;

    dispatch_append (fixture, "inbox:account-1", 1, message, &frame);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
    g_assert (strstr (frame.success.summary, "pairs=1:") != NULL);
    g_assert_true (wyrebox_daemon_delivery_materialization_catch_up
            (fixture->materialization, &error));
    g_assert_cmpuint (query_uint64 (fixture->catalog_path,
        "SELECT CAST(COUNT(*) AS UBIGINT) FROM mailbox_memberships "
        "WHERE mailbox_id = 'inbox:account-1' AND uid = 1 AND is_visible = "
        "TRUE;"), ==, 1);

    {
        g_autoptr (WyreboxJournalReader) reader = NULL;
        g_auto (WyreboxJournalRecord) record = { 0 };
        g_auto (WyreboxMessageAppendedPayload) payload = { 0 };
        g_autofree gchar *journal_root = NULL;
        gboolean eof = FALSE;

        journal_root = g_build_filename (fixture->root, "journal", NULL);
        reader = wyrebox_journal_reader_new (journal_root, &error);
        g_assert_true (wyrebox_journal_reader_read_next (reader, &record, &eof,
            &error));
        g_assert_cmpint (record.event_type, ==,
            WYREBOX_JOURNAL_EVENT_MESSAGE_APPENDED);
        g_assert_true (wyrebox_message_appended_payload_decode (record.payload,
            &payload, &error));
        stored = wyrebox_local_object_store_get_bytes (fixture->object_store,
                payload.object_key, &error);
        g_assert_nonnull (stored);
        g_assert_cmpuint (g_bytes_get_size (stored), ==, payload.size_bytes);
        g_assert_cmpuint (payload.uid, ==, 1);
    }
}

static void
test_unmaterialized_inbox_is_created (Fixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (GBytes) message = load_fixture_eml ();
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    g_autoptr (GError) error = NULL;

    dispatch_append (fixture, "inbox:account-1", 1, message, &frame);
    g_assert_true (wyrebox_daemon_delivery_materialization_catch_up
            (fixture->materialization, &error));
    g_assert_cmpuint (query_uint64 (fixture->catalog_path,
        "SELECT CAST(COUNT(*) AS UBIGINT) FROM mailboxes "
        "WHERE mailbox_id = 'inbox:account-1' AND imap_name = 'INBOX';"),
        ==, 1);
}

static void
test_virtual_destination_is_permission_denied (Fixture *fixture,
    gconstpointer user_data)
{
    duckdb_database database = NULL;
    duckdb_connection connection = NULL;
    g_autoptr (GBytes) message = load_fixture_eml ();
    g_auto (WyreboxDaemonMessageAppendRequest) request = { 0 };
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

    g_assert_true (wyrebox_daemon_message_append_request_init (&request,
        "account-1", "view-projects", 1, message, 0, NULL, NULL, &error));
    g_assert_true (wyrebox_daemon_message_append_dispatch (fixture->service,
        "request-append", "dovecot", "account-1", "dovecot-storage", "imap-1",
        &request, &frame, &error));
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
    g_assert_cmpint (frame.error.error_class, ==,
        WYREBOX_DAEMON_ERROR_PERMISSION_DENIED);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add ("/daemon-api/message-append-journal/creates-membership",
        Fixture, NULL, fixture_set_up,
        test_append_creates_membership_and_object, fixture_tear_down);
    g_test_add ("/daemon-api/message-append-journal/unmaterialized-inbox",
        Fixture, NULL, fixture_set_up, test_unmaterialized_inbox_is_created,
        fixture_tear_down);
    g_test_add ("/daemon-api/message-append-journal/virtual-dest-denied",
        Fixture, NULL, fixture_set_up,
        test_virtual_destination_is_permission_denied, fixture_tear_down);

    return g_test_run ();
}
