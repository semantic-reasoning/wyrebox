#include "wyrebox-daemon-delivery-materialization.h"
#include "wyrebox-daemon-flag-keyword-update-dispatcher.h"
#include "wyrebox-daemon-flag-keyword-update-journal.h"
#include "wyrebox-daemon-runtime.h"
#include "wyrebox-duckdb-shared.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

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
    WyreboxDaemonFlagKeywordUpdateService *service;
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
    g_assert_true (wyrebox_daemon_delivery_materialization_catch_up (
            materialization, &error));
    g_assert_no_error (error);

    return materialization;
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

    g_assert_nonnull (fixture_dir);
    path = g_build_filename (fixture_dir, fixture_name, NULL);
    g_assert_true (g_file_get_contents (path, &contents, &length, &error));
    g_assert_no_error (error);
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

    fixture->root = g_dir_make_tmp ("wyrebox-daemon-flag-update-XXXXXX",
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

    ingest (fixture, "simple-crlf.eml", "delivery-1", &fixture->first);
    ingest (fixture, "html-message.eml", "delivery-2", &fixture->second);

    fixture->catalog_path = prepare_catalog (fixture, "catalog.duckdb");
    fixture->materialization = new_materialization (fixture,
            fixture->catalog_path);
    fixture->service = wyrebox_daemon_flag_keyword_update_service_new_journaled
            (fixture->catalog_path, fixture->writer, fixture->materialization,
            &error);
    g_assert_no_error (error);
    g_assert_nonnull (fixture->service);
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
dispatch_update (const Fixture *fixture, guint64 uid_validity, guint64 uid,
    WyreboxDaemonFlagKeywordUpdateMode mode, const gchar *flags,
    const gchar *keywords, WyreboxDaemonResponseFrame *out_frame)
{
    g_auto (GStrv) system_flags = g_strsplit (flags, " ", -1);
    g_auto (GStrv) user_keywords = g_strsplit (keywords, " ", -1);
    g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (wyrebox_daemon_flag_keyword_update_request_init (&request,
        "account-1", "inbox:account-1", uid_validity, uid, mode,
        flags[0] != '\0' ? (const char *const *)system_flags : NULL,
        keywords[0] != '\0' ? (const char *const *)user_keywords : NULL,
        &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_flag_keyword_update_dispatch (
            fixture->service, "request-store", "dovecot", "account-1",
            "dovecot-storage", "imap-store-1", &request, out_frame, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (out_frame->request_id, ==, "request-store");
}

static guint64
update_ok (const Fixture *fixture, guint64 uid,
    WyreboxDaemonFlagKeywordUpdateMode mode, const gchar *flags,
    const gchar *keywords)
{
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

    dispatch_update (fixture, 1, uid, mode, flags, keywords, &frame);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_SUCCESS);
    g_assert_cmpuint (frame.success.journal_sequence, >, 0);
    g_assert_nonnull (frame.success.durable_marker);
    return frame.success.journal_offset;
}

static void
assert_update_fails (const Fixture *fixture, guint64 uid_validity,
    guint64 uid, WyreboxDaemonErrorClass error_class)
{
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    guint64 durable_end = wyrebox_journal_writer_get_durable_end
            (fixture->writer);

    dispatch_update (fixture, uid_validity, uid,
        WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET, "\\Seen", "", &frame);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
    g_assert_cmpint (frame.error.error_class, ==, error_class);
    g_assert_cmpuint (wyrebox_journal_writer_get_durable_end (fixture->writer),
        ==, durable_end);
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
    g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
    value = duckdb_value_varchar (&result, 0, 0);
    copy = g_strdup (value);
    duckdb_free (value);
    duckdb_destroy_result (&result);
    duckdb_disconnect (&connection);
    duckdb_close (&database);

    return copy;
}

static gchar *
flag_rows (const gchar *catalog_path)
{
    return query_catalog_string (catalog_path,
               "SELECT 'flags [' || COALESCE((SELECT string_agg(mm.uid || ' ' || "
               "f.flag_name, ', ' ORDER BY mm.uid, f.flag_name) "
               "FROM message_flags f JOIN mailbox_memberships mm "
               "ON mm.membership_id = f.membership_id), '') || '] keywords [' || "
               "COALESCE((SELECT string_agg(mm.uid || ' ' || k.keyword_name, ', ' "
               "ORDER BY mm.uid, k.keyword_name) FROM message_keywords k "
               "JOIN mailbox_memberships mm ON mm.membership_id = k.membership_id), "
               "'') || ']';");
}

static void
assert_flag_rows (const Fixture *fixture, const gchar *expected)
{
    g_autofree gchar *actual = flag_rows (fixture->catalog_path);

    g_assert_cmpstr (actual, ==, expected);
}

static void
test_update_modes_materialize_flags_and_keywords (Fixture *fixture,
    gconstpointer user_data)
{
    guint64 offset = 0;

    offset = update_ok (fixture, 1, WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
            "\\Seen \\Flagged", "work");
    g_assert_cmpuint (offset, >, fixture->second.journal_offset);
    assert_flag_rows (fixture,
        "flags [1 \\Flagged, 1 \\Seen] keywords [1 work]");

    update_ok (fixture, 2, WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
        "\\Answered", "");
    update_ok (fixture, 1, WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR,
        "\\Flagged", "work");
    assert_flag_rows (fixture,
        "flags [1 \\Seen, 2 \\Answered] keywords []");

    update_ok (fixture, 1, WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE,
        "\\Draft", "later urgent");
    assert_flag_rows (fixture,
        "flags [1 \\Draft, 2 \\Answered] keywords [1 later, 1 urgent]");

    update_ok (fixture, 1, WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE,
        "", "");
    assert_flag_rows (fixture, "flags [2 \\Answered] keywords []");
}

static void
test_update_writes_one_flag_changed_record (Fixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (WyreboxJournalReader) reader = NULL;
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxJournalRecord) record = { 0 };
    gboolean eof = FALSE;
    guint flag_records = 0;
    guint64 offset = 0;
    guint64 last_offset = 0;

    offset = update_ok (fixture, 2,
            WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE, "\\Seen", "work");

    reader = wyrebox_journal_reader_new (fixture->journal_root, &error);
    g_assert_no_error (error);
    while (wyrebox_journal_reader_read_next (reader, &record, &eof, &error)) {
        g_assert_cmpint (record.event_type, !=,
            WYREBOX_JOURNAL_EVENT_KEYWORD_CHANGED);
        if (record.event_type == WYREBOX_JOURNAL_EVENT_FLAG_CHANGED) {
            flag_records++;
            last_offset = record.offset;
        }
    }
    g_assert_true (eof);
    g_assert_cmpuint (flag_records, ==, 1);
    g_assert_cmpuint (last_offset, ==, offset);
}

static void
test_invalid_flags_are_rejected (Fixture *fixture, gconstpointer user_data)
{
    const char *const recent[] = { "\\Recent", NULL };
    const char *const system_keyword[] = { "\\Seen", NULL };
    g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_false (wyrebox_daemon_flag_keyword_update_request_init (&request,
        "account-1", "inbox:account-1", 1, 1,
        WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET, recent, NULL, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error (&error);

    g_assert_false (wyrebox_daemon_flag_keyword_update_request_init (&request,
        "account-1", "inbox:account-1", 1, 1,
        WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET, NULL, system_keyword,
        &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
}

static void
test_unknown_uid_and_stale_uidvalidity_fail (Fixture *fixture,
    gconstpointer user_data)
{
    assert_update_fails (fixture, 1, 3, WYREBOX_DAEMON_ERROR_NOT_FOUND);
    assert_update_fails (fixture, 2, 1, WYREBOX_DAEMON_ERROR_CONFLICT);
    assert_flag_rows (fixture, "flags [] keywords []");
}

static void
test_unknown_mailbox_is_not_found (Fixture *fixture, gconstpointer user_data)
{
    g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    g_autoptr (GError) error = NULL;
    const char *const seen[] = { "\\Seen", NULL };

    g_assert_true (wyrebox_daemon_flag_keyword_update_request_init (&request,
        "account-1", "missing-mailbox", 1, 1,
        WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET, seen, NULL, &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_flag_keyword_update_dispatch (
            fixture->service, "request-store", "dovecot", "account-1",
            "dovecot-storage", "imap-store-1", &request, &frame, &error));
    g_assert_no_error (error);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
    g_assert_cmpint (frame.error.error_class, ==,
        WYREBOX_DAEMON_ERROR_NOT_FOUND);
}

static gchar *
raw_object_path (const gchar *object_root, const gchar *object_key)
{
    const gchar *hex = object_key + strlen ("sha256:");
    g_autofree gchar *prefix = g_strndup (hex, 2);
    g_autofree gchar *filename = g_strdup_printf ("%s.eml", hex);

    return g_build_filename (object_root, "objects", "sha256", prefix,
               filename, NULL);
}

typedef struct
{
    GBytes *bytes;
    GStatBuf stat;
} RawObjectSnapshot;

static void
snapshot_raw_object (const Fixture *fixture, const gchar *object_key,
    RawObjectSnapshot *out)
{
    g_autofree gchar *path = raw_object_path (fixture->object_root,
            object_key);
    g_autoptr (GError) error = NULL;
    gchar *contents = NULL;
    gsize length = 0;

    g_assert_true (g_file_get_contents (path, &contents, &length, &error));
    g_assert_no_error (error);
    out->bytes = g_bytes_new_take (contents, length);
    g_assert_cmpint (g_stat (path, &out->stat), ==, 0);
}

static void
assert_raw_object_unchanged (const Fixture *fixture, const gchar *object_key,
    const RawObjectSnapshot *before)
{
    RawObjectSnapshot after = { 0 };

    snapshot_raw_object (fixture, object_key, &after);
    g_assert_true (g_bytes_equal (after.bytes, before->bytes));
    g_assert_cmpuint (after.stat.st_ino, ==, before->stat.st_ino);
    g_assert_cmpint (after.stat.st_mtime, ==, before->stat.st_mtime);
    g_assert_cmpint (after.stat.st_mode, ==, before->stat.st_mode);
    g_bytes_unref (after.bytes);
}

static const gchar *object_metadata_sql =
    "SELECT string_agg(o.object_id || ' ' || o.size_bytes || ' ' || "
    "m.message_id || ' ' || m.journal_offset, ', ' ORDER BY m.message_id) "
    "FROM objects o JOIN messages m ON m.object_id = o.object_id;";

static void
test_updates_leave_raw_objects_unchanged (Fixture *fixture,
    gconstpointer user_data)
{
    RawObjectSnapshot first = { 0 };
    RawObjectSnapshot second = { 0 };
    g_autofree gchar *metadata_before = NULL;
    g_autofree gchar *metadata_after = NULL;

    snapshot_raw_object (fixture, fixture->first.object_key, &first);
    snapshot_raw_object (fixture, fixture->second.object_key, &second);
    metadata_before = query_catalog_string (fixture->catalog_path,
            object_metadata_sql);

    update_ok (fixture, 1, WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
        "\\Seen \\Deleted", "work");
    update_ok (fixture, 2, WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE,
        "\\Flagged", "later");
    update_ok (fixture, 1, WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR,
        "\\Deleted", "");

    assert_raw_object_unchanged (fixture, fixture->first.object_key, &first);
    assert_raw_object_unchanged (fixture, fixture->second.object_key,
        &second);
    metadata_after = query_catalog_string (fixture->catalog_path,
            object_metadata_sql);
    g_assert_cmpstr (metadata_after, ==, metadata_before);

    g_bytes_unref (first.bytes);
    g_bytes_unref (second.bytes);
}

static void
test_rebuilt_catalog_replays_flags (Fixture *fixture, gconstpointer user_data)
{
    g_autofree gchar *rebuilt_path = NULL;
    g_autofree gchar *expected = NULL;
    g_autofree gchar *rebuilt = NULL;
    g_autoptr (WyreboxDaemonDeliveryMaterialization) materialization = NULL;

    update_ok (fixture, 1, WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
        "\\Seen \\Flagged", "work later");
    update_ok (fixture, 2, WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
        "\\Answered", "");
    update_ok (fixture, 1, WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR,
        "\\Flagged", "later");
    update_ok (fixture, 2, WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE,
        "\\Draft", "urgent");
    expected = flag_rows (fixture->catalog_path);
    g_assert_cmpstr (expected, ==,
        "flags [1 \\Seen, 2 \\Draft] keywords [1 work, 2 urgent]");

    rebuilt_path = prepare_catalog (fixture, "rebuilt.duckdb");
    materialization = new_materialization (fixture, rebuilt_path);
    wyrebox_daemon_delivery_materialization_stop (materialization);
    rebuilt = flag_rows (rebuilt_path);
    g_assert_cmpstr (rebuilt, ==, expected);
}

#define ADD_TEST(path, func) \
        g_test_add ("/daemon-api/flag-keyword-update-journal/" path, Fixture, \
            NULL, fixture_set_up, func, fixture_tear_down)

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    ADD_TEST ("modes", test_update_modes_materialize_flags_and_keywords);
    ADD_TEST ("one-record", test_update_writes_one_flag_changed_record);
    ADD_TEST ("invalid-flags", test_invalid_flags_are_rejected);
    ADD_TEST ("unknown-uid-stale-uidvalidity",
        test_unknown_uid_and_stale_uidvalidity_fail);
    ADD_TEST ("unknown-mailbox", test_unknown_mailbox_is_not_found);
    ADD_TEST ("raw-objects-unchanged",
        test_updates_leave_raw_objects_unchanged);
    ADD_TEST ("rebuild", test_rebuilt_catalog_replays_flags);

    return g_test_run ();
}
