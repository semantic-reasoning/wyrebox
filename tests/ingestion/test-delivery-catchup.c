#include "wyrebox-daemon-fact-mutation-request.h"
#include "wyrebox-delivery-catchup.h"
#include "wyrebox-delivery-materializer.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-flag-changed-payload.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-message-delivered-payload.h"
#include "wyrebox-schema-metadata-store.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#define JOURNAL_SEGMENT_NAME "00000000000000000000.wbj"

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
journal_segment_path (const gchar *journal_root)
{
    return g_build_filename (journal_root, JOURNAL_SEGMENT_NAME, NULL);
}

static void
read_journal_segment (const gchar *journal_root,
    guint8 **out_data, gsize *out_size)
{
    g_autoptr (GError) error = NULL;
    g_autofree gchar *segment_path = journal_segment_path (journal_root);
    g_autofree gchar *contents = NULL;

    g_assert_true (g_file_get_contents (segment_path, &contents, out_size,
        &error));
    g_assert_no_error (error);
    g_assert_nonnull (contents);

    *out_data = (guint8 *)g_steal_pointer (&contents);
}

static void
write_journal_segment (const gchar *journal_root, guint8 *data, gsize size)
{
    g_autoptr (GError) error = NULL;
    g_autofree gchar *segment_path = journal_segment_path (journal_root);

    g_assert_true (g_file_set_contents (segment_path, (const gchar *)data,
        (gssize)size, &error));
    g_assert_no_error (error);
}

static void
truncate_journal_segment (const gchar *journal_root, guint64 size)
{
    g_autofree gchar *segment_path = journal_segment_path (journal_root);

    g_assert_cmpuint (size, <=, G_MAXSSIZE);
    g_assert_cmpint (truncate (segment_path, (off_t)size), ==, 0);
}

static void
corrupt_journal_checksum (const gchar *journal_root, guint64 record_offset)
{
    g_autofree guint8 *segment = NULL;
    gsize segment_size = 0;

    read_journal_segment (journal_root, &segment, &segment_size);
    g_assert_cmpuint (segment_size, >, record_offset + 32);
    segment[record_offset + 32] ^= 0x01;
    write_journal_segment (journal_root, segment, segment_size);
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

static GBytes *
load_fixture_bytes (const char *fixture_dir, const char *name)
{
    g_autoptr (GError) error = NULL;
    g_autofree char *path = g_build_filename (fixture_dir, name, NULL);
    g_autofree char *contents = NULL;
    gsize length = 0;

    g_assert_true (g_file_get_contents (path, &contents, &length, &error));
    g_assert_no_error (error);

    return g_bytes_new_take (g_steal_pointer (&contents), length);
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

static guint64
query_uint64 (duckdb_connection connection, const gchar *sql)
{
    g_auto (duckdb_result) result = { 0 };

    g_assert_cmpint (duckdb_query (connection, sql, &result), ==,
        DuckDBSuccess);
    g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
    g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
    g_assert_false (duckdb_value_is_null (&result, 0, 0));

    return (guint64)duckdb_value_uint64 (&result, 0, 0);
}

static gchar *
query_string (duckdb_connection connection, const gchar *sql)
{
    g_auto (duckdb_result) result = { 0 };
    g_auto (TestDuckdbOwnedString) value = NULL;

    g_assert_cmpint (duckdb_query (connection, sql, &result), ==,
        DuckDBSuccess);
    g_assert_cmpuint (duckdb_column_count (&result), ==, 1);
    g_assert_cmpuint (duckdb_row_count (&result), ==, 1);
    g_assert_false (duckdb_value_is_null (&result, 0, 0));

    value = duckdb_value_varchar (&result, 0, 0);
    return g_strdup (value);
}

static void
assert_table_count (duckdb_connection connection, const gchar *table,
    guint64 expected)
{
    g_autofree gchar *sql = g_strdup_printf ("SELECT COUNT(*) FROM %s;", table);

    g_assert_cmpuint (query_uint64 (connection, sql), ==, expected);
}

static void
assert_materialization_checkpoint (duckdb_connection connection,
    guint64 expected_offset, guint64 expected_sequence)
{
    g_assert_cmpuint (query_uint64 (connection,
        "SELECT COUNT(*) FROM materialization_checkpoint WHERE "
        "checkpoint_key = 'materialization';"), ==, 1);
    g_assert_cmpuint (query_uint64 (connection,
        "SELECT journal_offset FROM materialization_checkpoint WHERE "
        "checkpoint_key = 'materialization';"), ==, expected_offset);
    g_assert_cmpuint (query_uint64 (connection,
        "SELECT journal_sequence FROM materialization_checkpoint WHERE "
        "checkpoint_key = 'materialization';"), ==, expected_sequence);
}

static void
assert_unmaterialized_state (const gchar *catalog_path)
{
    TestDuckdbFixture duckdb = { 0 };

    open_duckdb_fixture (catalog_path, &duckdb);
    assert_table_count (duckdb.connection, "messages", 0);
    assert_table_count (duckdb.connection, "message_headers", 0);
    assert_table_count (duckdb.connection, "mailbox_memberships", 0);
    assert_table_count (duckdb.connection, "mailboxes", 0);
    assert_table_count (duckdb.connection, "mailbox_uid_state", 0);
    assert_table_count (duckdb.connection, "materialization_checkpoint", 0);
    close_duckdb_fixture (&duckdb);
}

static gchar *
create_bootstrap_catalog (void)
{
    g_autofree gchar *dir = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

    dir = g_dir_make_tmp ("wyrebox-delivery-catchup-XXXXXX", &error);
    g_assert_no_error (error);
    g_assert_nonnull (dir);

    path = g_build_filename (dir, "catalog.duckdb", NULL);
    store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
    g_assert_no_error (error);
    g_assert_nonnull (store);
    g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (
            store,
            WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
            0, 1, &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (
            store,
            WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_ATTRIBUTE_TABLES,
            wyrebox_schema_migration_get_first_supported_schema_version (), 2,
            &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (
            store,
            WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_TABLE,
            2, 3, &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (
            store,
            WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_SENDER_DOMAIN,
            5, 6, &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (
            store,
            WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_DATE_UNIX_US,
            6, wyrebox_schema_migration_get_current_schema_version (), &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (
            store,
            WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_PROVENANCE_SPANS,
            8, wyrebox_schema_migration_get_current_schema_version (), &error));
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
ingest_fixture (WyreboxEmlIngestor *ingestor,
    const gchar *fixture_name, WyreboxEmlIngestResult *out_result)
{
    const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) input = NULL;

    g_assert_true (WYREBOX_IS_EML_INGESTOR (ingestor));
    g_assert_nonnull (fixture_dir);

    input = load_fixture_bytes (fixture_dir, fixture_name);
    g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, input,
        out_result, &error));
    g_assert_no_error (error);
}

static WyreboxEmlIngestor *
create_ingestor (const gchar *object_root,
    const gchar *journal_root,
    WyreboxLocalObjectStore **out_object_store,
    WyreboxJournalWriter **out_writer)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

    object_store = wyrebox_local_object_store_new (object_root, &error);
    g_assert_no_error (error);
    g_assert_nonnull (object_store);
    writer = wyrebox_journal_writer_new (journal_root, &error);
    g_assert_no_error (error);
    g_assert_nonnull (writer);
    ingestor = wyrebox_eml_ingestor_new_with_journal (object_store, writer);
    g_assert_nonnull (ingestor);

    *out_object_store = g_steal_pointer (&object_store);
    *out_writer = g_steal_pointer (&writer);
    return g_steal_pointer (&ingestor);
}

static gboolean
run_catchup (const gchar *catalog_path,
    const gchar *object_root, const gchar *journal_root, GError **error)
{
    g_autoptr (WyreboxSchemaMetadataStore) metadata_store = NULL;
    g_autoptr (WyreboxJournalReader) reader = NULL;
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxDeliveryMaterializer) materializer = NULL;

    metadata_store = wyrebox_schema_metadata_store_new_duckdb (catalog_path,
            error);
    if (metadata_store == NULL)
        return FALSE;
    reader = wyrebox_journal_reader_new (journal_root, error);
    if (reader == NULL)
        return FALSE;
    object_store = wyrebox_local_object_store_new (object_root, error);
    if (object_store == NULL)
        return FALSE;
    materializer = wyrebox_delivery_materializer_new_duckdb (catalog_path,
            error);
    if (materializer == NULL)
        return FALSE;

    return wyrebox_delivery_catchup_materialize_inbox (metadata_store, reader,
               object_store, materializer, "account-1", error);
}

static void
assert_inbox_state (const gchar *catalog_path,
    guint64 expected_messages,
    guint64 expected_uidnext,
    guint64 expected_checkpoint_offset, guint64 expected_checkpoint_sequence)
{
    TestDuckdbFixture duckdb = { 0 };
    g_autofree gchar *mailbox_name = NULL;

    open_duckdb_fixture (catalog_path, &duckdb);
    assert_table_count (duckdb.connection, "messages", expected_messages);
    assert_table_count (duckdb.connection, "message_headers",
        expected_messages);
    assert_table_count (duckdb.connection, "mailbox_memberships",
        expected_messages);
    assert_materialization_checkpoint (duckdb.connection,
        expected_checkpoint_offset, expected_checkpoint_sequence);
    mailbox_name = query_string (duckdb.connection,
            "SELECT imap_name FROM mailboxes WHERE mailbox_id = 'mailbox-inbox' "
            "AND account_id = 'account-1';");
    g_assert_cmpstr (mailbox_name, ==, "INBOX");
    g_assert_cmpuint (query_uint64 (duckdb.connection,
        "SELECT uidnext FROM mailbox_uid_state WHERE "
        "account_id = 'account-1' AND namespace_kind = 'mailbox' "
        "AND namespace_id = 'mailbox-inbox';"), ==, expected_uidnext);
    close_duckdb_fixture (&duckdb);
}

static void
assert_membership_uid (const gchar *catalog_path,
    const WyreboxEmlIngestResult *result, guint64 expected_uid)
{
    TestDuckdbFixture duckdb = { 0 };
    g_autofree gchar *sql = NULL;

    sql =
        g_strdup_printf
            ("SELECT uid FROM mailbox_memberships WHERE journal_offset = %"
            G_GUINT64_FORMAT " AND journal_sequence = %" G_GUINT64_FORMAT ";",
            result->journal_offset, result->journal_sequence);

    open_duckdb_fixture (catalog_path, &duckdb);
    g_assert_cmpuint (query_uint64 (duckdb.connection, sql), ==, expected_uid);
    close_duckdb_fixture (&duckdb);
}

static void
save_wrong_checkpoint (const gchar *catalog_path,
    const WyreboxEmlIngestResult *first_result)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxSchemaMetadataStore) metadata_store = NULL;
    g_auto (WyreboxSchemaMigrationMetadataState) metadata = { 0 };

    metadata_store = wyrebox_schema_metadata_store_new_duckdb (catalog_path,
            &error);
    g_assert_no_error (error);
    g_assert_nonnull (metadata_store);

    metadata.materialization_checkpoint_present = TRUE;
    metadata.materialization_checkpoint_journal_offset =
        first_result->journal_offset;
    metadata.materialization_checkpoint_sequence =
        first_result->journal_sequence + 100;

    g_assert_true (wyrebox_schema_metadata_store_save (metadata_store,
        &metadata, &error));
    g_assert_no_error (error);
}

static void
assert_unsafe_suffix_error (GError *error,
    const gchar *reason,
    guint64 unsafe_offset, guint64 safe_end_offset,
    gboolean has_last_safe_sequence, guint64 last_safe_sequence)
{
    g_autofree gchar *unsafe_offset_fragment =
        g_strdup_printf ("unsafe offset %" G_GUINT64_FORMAT, unsafe_offset);
    g_autofree gchar *safe_end_offset_fragment =
        g_strdup_printf ("safe end offset %" G_GUINT64_FORMAT, safe_end_offset);
    g_autofree gchar *last_safe_sequence_fragment = NULL;

    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "journal unsafe suffix found"));
    g_assert_nonnull (strstr (error->message, "stop reason"));
    g_assert_nonnull (strstr (error->message, reason));
    g_assert_nonnull (strstr (error->message, unsafe_offset_fragment));
    g_assert_nonnull (strstr (error->message, safe_end_offset_fragment));

    if (has_last_safe_sequence) {
        last_safe_sequence_fragment =
            g_strdup_printf ("last safe sequence %" G_GUINT64_FORMAT,
                last_safe_sequence);
    } else {
        last_safe_sequence_fragment = g_strdup ("last safe sequence none");
    }
    g_assert_nonnull (strstr (error->message, last_safe_sequence_fragment));
}

static void
test_no_checkpoint_materializes_two_deliveries (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_auto (WyreboxEmlIngestResult) second = { 0 };

    g_assert_nonnull (object_root);
    g_assert_nonnull (journal_root);

    ingestor = create_ingestor (object_root, journal_root,
            &ingest_object_store, &writer);
    ingest_fixture (ingestor, "simple-crlf.eml", &first);
    ingest_fixture (ingestor, "duplicate-message-id.eml", &second);

    g_assert_true (run_catchup (catalog_path, object_root, journal_root,
        &error));
    g_assert_no_error (error);

    assert_inbox_state (catalog_path, 2, 3, second.journal_offset,
        second.journal_sequence);
    assert_membership_uid (catalog_path, &first, 1);
    assert_membership_uid (catalog_path, &second, 2);

    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

static void
test_partial_trailing_record_fails_without_materializing (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_auto (WyreboxEmlIngestResult) second = { 0 };
    g_auto (WyreboxEmlIngestResult) third = { 0 };

    g_assert_nonnull (object_root);
    g_assert_nonnull (journal_root);

    ingestor = create_ingestor (object_root, journal_root,
            &ingest_object_store, &writer);
    ingest_fixture (ingestor, "simple-crlf.eml", &first);
    ingest_fixture (ingestor, "duplicate-message-id.eml", &second);
    ingest_fixture (ingestor, "html-message.eml", &third);
    g_clear_object (&ingestor);
    g_clear_object (&writer);

    truncate_journal_segment (journal_root, third.journal_offset + 17);

    g_assert_false (run_catchup (catalog_path, object_root, journal_root,
        &error));
    assert_unsafe_suffix_error (error, "partial-header", third.journal_offset,
        third.journal_offset, TRUE, second.journal_sequence);
    assert_unmaterialized_state (catalog_path);

    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

static void
test_checksum_mismatch_fails_without_materializing (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_auto (WyreboxEmlIngestResult) second = { 0 };
    g_auto (WyreboxEmlIngestResult) third = { 0 };

    g_assert_nonnull (object_root);
    g_assert_nonnull (journal_root);

    ingestor = create_ingestor (object_root, journal_root,
            &ingest_object_store, &writer);
    ingest_fixture (ingestor, "simple-crlf.eml", &first);
    ingest_fixture (ingestor, "duplicate-message-id.eml", &second);
    ingest_fixture (ingestor, "html-message.eml", &third);
    g_clear_object (&ingestor);
    g_clear_object (&writer);

    corrupt_journal_checksum (journal_root, third.journal_offset);

    g_assert_false (run_catchup (catalog_path, object_root, journal_root,
        &error));
    assert_unsafe_suffix_error (error, "checksum-mismatch",
        third.journal_offset, third.journal_offset, TRUE,
        second.journal_sequence);
    assert_unmaterialized_state (catalog_path);

    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

static void
test_first_record_corruption_fails_without_materializing (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_auto (WyreboxEmlIngestResult) second = { 0 };

    g_assert_nonnull (object_root);
    g_assert_nonnull (journal_root);

    ingestor = create_ingestor (object_root, journal_root,
            &ingest_object_store, &writer);
    ingest_fixture (ingestor, "simple-crlf.eml", &first);
    ingest_fixture (ingestor, "duplicate-message-id.eml", &second);
    g_clear_object (&ingestor);
    g_clear_object (&writer);

    corrupt_journal_checksum (journal_root, first.journal_offset);

    g_assert_false (run_catchup (catalog_path, object_root, journal_root,
        &error));
    assert_unsafe_suffix_error (error, "checksum-mismatch",
        first.journal_offset, 0, FALSE, 0);
    assert_unmaterialized_state (catalog_path);

    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

static void
test_existing_checkpoint_materializes_suffix (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_auto (WyreboxEmlIngestResult) second = { 0 };
    g_auto (WyreboxEmlIngestResult) third = { 0 };

    g_assert_nonnull (object_root);
    g_assert_nonnull (journal_root);

    ingestor = create_ingestor (object_root, journal_root,
            &ingest_object_store, &writer);
    ingest_fixture (ingestor, "simple-crlf.eml", &first);
    ingest_fixture (ingestor, "duplicate-message-id.eml", &second);

    g_assert_true (run_catchup (catalog_path, object_root, journal_root,
        &error));
    g_assert_no_error (error);
    assert_inbox_state (catalog_path, 2, 3, second.journal_offset,
        second.journal_sequence);

    ingest_fixture (ingestor, "html-message.eml", &third);

    g_assert_true (run_catchup (catalog_path, object_root, journal_root,
        &error));
    g_assert_no_error (error);

    assert_inbox_state (catalog_path, 3, 4, third.journal_offset,
        third.journal_sequence);
    assert_membership_uid (catalog_path, &first, 1);
    assert_membership_uid (catalog_path, &second, 2);
    assert_membership_uid (catalog_path, &third, 3);

    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

static void
test_checkpoint_at_eof_leaves_state_unchanged (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_auto (WyreboxEmlIngestResult) second = { 0 };

    g_assert_nonnull (object_root);
    g_assert_nonnull (journal_root);

    ingestor = create_ingestor (object_root, journal_root,
            &ingest_object_store, &writer);
    ingest_fixture (ingestor, "simple-crlf.eml", &first);
    ingest_fixture (ingestor, "duplicate-message-id.eml", &second);

    g_assert_true (run_catchup (catalog_path, object_root, journal_root,
        &error));
    g_assert_no_error (error);
    g_assert_true (run_catchup (catalog_path, object_root, journal_root,
        &error));
    g_assert_no_error (error);

    assert_inbox_state (catalog_path, 2, 3, second.journal_offset,
        second.journal_sequence);
    assert_membership_uid (catalog_path, &first, 1);
    assert_membership_uid (catalog_path, &second, 2);

    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

static void
test_bad_checkpoint_seek_fails_without_materializing (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_auto (WyreboxEmlIngestResult) second = { 0 };
    TestDuckdbFixture duckdb = { 0 };

    g_assert_nonnull (object_root);
    g_assert_nonnull (journal_root);

    ingestor = create_ingestor (object_root, journal_root,
            &ingest_object_store, &writer);
    ingest_fixture (ingestor, "simple-crlf.eml", &first);
    ingest_fixture (ingestor, "duplicate-message-id.eml", &second);
    save_wrong_checkpoint (catalog_path, &first);

    g_assert_false (run_catchup (catalog_path, object_root, journal_root,
        &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

    open_duckdb_fixture (catalog_path, &duckdb);
    assert_table_count (duckdb.connection, "messages", 0);
    assert_table_count (duckdb.connection, "mailbox_memberships", 0);
    assert_materialization_checkpoint (duckdb.connection, first.journal_offset,
        first.journal_sequence + 100);
    close_duckdb_fixture (&duckdb);

    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

static void
ingest_delivery_fixture (WyreboxEmlIngestor *ingestor,
    const gchar *fixture_name, const gchar *delivery_id,
    const gchar *account_id, WyreboxEmlIngestResult *out_result)
{
    const char *fixture_dir = g_getenv ("WYREBOX_EML_FIXTURE_DIR");
    const gchar *const recipients[] = { "user@example.test", NULL };
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) input = NULL;

    g_assert_nonnull (fixture_dir);

    input = load_fixture_bytes (fixture_dir, fixture_name);
    g_assert_true (wyrebox_eml_ingestor_ingest_delivery_bytes (ingestor, input,
        delivery_id, NULL, account_id, "sender@example.test", recipients,
        out_result, &error));
    g_assert_no_error (error);
}

static gboolean
run_account_catchup (const gchar *catalog_path,
    const gchar *object_root, const gchar *journal_root, GError **error)
{
    g_autoptr (WyreboxSchemaMetadataStore) metadata_store = NULL;
    g_autoptr (WyreboxJournalReader) reader = NULL;
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxDeliveryMaterializer) materializer = NULL;

    metadata_store = wyrebox_schema_metadata_store_new_duckdb (catalog_path,
            error);
    if (metadata_store == NULL)
        return FALSE;
    reader = wyrebox_journal_reader_new (journal_root, error);
    if (reader == NULL)
        return FALSE;
    object_store = wyrebox_local_object_store_new (object_root, error);
    if (object_store == NULL)
        return FALSE;
    materializer = wyrebox_delivery_materializer_new_duckdb (catalog_path,
            error);
    if (materializer == NULL)
        return FALSE;

    return wyrebox_delivery_catchup_materialize_account_inboxes (
        metadata_store, reader, object_store, materializer, error);
}

static guint64
account_inbox_uid (const gchar *catalog_path, const gchar *account_id,
    const WyreboxEmlIngestResult *result)
{
    TestDuckdbFixture duckdb = { 0 };
    g_autofree gchar *sql = NULL;
    guint64 uid = 0;

    sql = g_strdup_printf ("SELECT m.uid FROM mailbox_memberships m "
            "JOIN mailboxes b ON b.mailbox_id = m.mailbox_id "
            "WHERE b.account_id = '%s' AND b.imap_name = 'INBOX' "
            "AND m.journal_offset = %" G_GUINT64_FORMAT
            " AND m.journal_sequence = %" G_GUINT64_FORMAT ";",
            account_id, result->journal_offset, result->journal_sequence);

    open_duckdb_fixture (catalog_path, &duckdb);
    uid = query_uint64 (duckdb.connection, sql);
    close_duckdb_fixture (&duckdb);

    return uid;
}

static void
test_account_catchup_routes_deliveries_by_account (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_auto (WyreboxEmlIngestResult) second = { 0 };
    g_auto (WyreboxEmlIngestResult) third = { 0 };
    TestDuckdbFixture duckdb = { 0 };

    ingestor = create_ingestor (object_root, journal_root,
            &ingest_object_store, &writer);
    ingest_delivery_fixture (ingestor, "simple-crlf.eml", "delivery-1",
        "account-a", &first);
    ingest_delivery_fixture (ingestor, "duplicate-message-id.eml",
        "delivery-2", "account-b", &second);
    ingest_delivery_fixture (ingestor, "simple-crlf.eml", "delivery-3",
        "account-a", &third);

    g_assert_true (run_account_catchup (catalog_path, object_root,
        journal_root, &error));
    g_assert_no_error (error);
    g_assert_true (run_account_catchup (catalog_path, object_root,
        journal_root, &error));
    g_assert_no_error (error);

    g_assert_cmpuint (account_inbox_uid (catalog_path, "account-a", &first),
        ==, 1);
    g_assert_cmpuint (account_inbox_uid (catalog_path, "account-b", &second),
        ==, 1);
    g_assert_cmpuint (account_inbox_uid (catalog_path, "account-a", &third),
        ==, 2);

    open_duckdb_fixture (catalog_path, &duckdb);
    assert_table_count (duckdb.connection, "mailboxes", 2);
    assert_table_count (duckdb.connection, "mailbox_memberships", 3);
    assert_materialization_checkpoint (duckdb.connection, third.journal_offset,
        third.journal_sequence);
    close_duckdb_fixture (&duckdb);

    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

static void
test_account_catchup_rejects_delivery_without_account (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_auto (WyreboxEmlIngestResult) second = { 0 };
    g_autofree gchar *location = NULL;

    ingestor = create_ingestor (object_root, journal_root,
            &ingest_object_store, &writer);
    ingest_delivery_fixture (ingestor, "simple-crlf.eml", "delivery-1",
        "account-a", &first);
    ingest_fixture (ingestor, "duplicate-message-id.eml", &second);

    g_assert_false (run_account_catchup (catalog_path, object_root,
        journal_root, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    location = g_strdup_printf ("offset %" G_GUINT64_FORMAT,
            second.journal_offset);
    g_assert_nonnull (strstr (error->message, "account identity"));
    g_assert_nonnull (strstr (error->message, location));
    assert_unmaterialized_state (catalog_path);

    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

static void
test_account_catchup_rebuild_keeps_uids (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autofree gchar *rebuilt_path = create_bootstrap_catalog ();
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) ingest_object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_auto (WyreboxEmlIngestResult) second = { 0 };
    g_auto (WyreboxEmlIngestResult) third = { 0 };

    ingestor = create_ingestor (object_root, journal_root,
            &ingest_object_store, &writer);
    ingest_delivery_fixture (ingestor, "simple-crlf.eml", "delivery-1",
        "account-a", &first);
    ingest_delivery_fixture (ingestor, "duplicate-message-id.eml",
        "delivery-2", "account-a", &second);

    g_assert_true (run_account_catchup (catalog_path, object_root,
        journal_root, &error));
    g_assert_no_error (error);

    ingest_delivery_fixture (ingestor, "simple-crlf.eml", "delivery-3",
        "account-a", &third);
    g_assert_true (run_account_catchup (catalog_path, object_root,
        journal_root, &error));
    g_assert_no_error (error);

    g_assert_true (run_account_catchup (rebuilt_path, object_root,
        journal_root, &error));
    g_assert_no_error (error);

    g_assert_cmpuint (account_inbox_uid (catalog_path, "account-a", &third),
        ==, 3);
    g_assert_cmpuint (account_inbox_uid (rebuilt_path, "account-a", &first),
        ==, account_inbox_uid (catalog_path, "account-a", &first));
    g_assert_cmpuint (account_inbox_uid (rebuilt_path, "account-a", &second),
        ==, account_inbox_uid (catalog_path, "account-a", &second));
    g_assert_cmpuint (account_inbox_uid (rebuilt_path, "account-a", &third),
        ==, account_inbox_uid (catalog_path, "account-a", &third));

    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
    remove_catalog (rebuilt_path);
}

static void
execute_catalog_sql (const gchar *catalog_path, const gchar *sql)
{
    TestDuckdbFixture duckdb = { 0 };
    g_auto (duckdb_result) result = { 0 };

    open_duckdb_fixture (catalog_path, &duckdb);
    g_assert_cmpint (duckdb_query (duckdb.connection, sql, &result), ==,
        DuckDBSuccess);
    close_duckdb_fixture (&duckdb);
}

static void
seed_account_b_inbox (const gchar *catalog_path, gboolean selectable)
{
    g_autofree gchar *sql = NULL;

    execute_catalog_sql (catalog_path,
        "INSERT INTO accounts (account_id) VALUES ('account-b');");
    sql = g_strdup_printf ("INSERT INTO mailboxes ("
            "mailbox_id, account_id, imap_name, is_selectable, is_visible"
            ") VALUES ('inbox-b', 'account-b', 'INBOX', %s, TRUE);",
            selectable ? "TRUE" : "FALSE");
    execute_catalog_sql (catalog_path, sql);
}

static void
make_account_b_inbox_selectable_in (const gchar *catalog_path)
{
    execute_catalog_sql (catalog_path,
        "UPDATE mailboxes SET is_selectable = TRUE "
        "WHERE mailbox_id = 'inbox-b';");
}

static gboolean
run_isolated_catchup (const gchar *catalog_path, const gchar *object_root,
    const gchar *journal_root, WyreboxDeliveryCatchupReport *out_report,
    GError **error)
{
    g_autoptr (WyreboxSchemaMetadataStore) metadata_store = NULL;
    g_autoptr (WyreboxJournalReader) reader = NULL;
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxDeliveryMaterializer) materializer = NULL;

    metadata_store = wyrebox_schema_metadata_store_new_duckdb (catalog_path,
            error);
    if (metadata_store == NULL)
        return FALSE;
    reader = wyrebox_journal_reader_new (journal_root, error);
    if (reader == NULL)
        return FALSE;
    object_store = wyrebox_local_object_store_new (object_root, error);
    if (object_store == NULL)
        return FALSE;
    materializer = wyrebox_delivery_materializer_new_duckdb (catalog_path,
            error);
    if (materializer == NULL)
        return FALSE;

    return wyrebox_delivery_catchup_materialize_account_inboxes_isolated (
        metadata_store, reader, object_store, materializer, out_report, error);
}

static gboolean
run_resumed_catchup (const gchar *catalog_path, const gchar *object_root,
    const gchar *journal_root, const WyreboxDeliveryCatchupCursor *resume_after,
    const GPtrArray *prior_holds, WyreboxDeliveryCatchupReport *out_report,
    GError **error)
{
    g_autoptr (WyreboxSchemaMetadataStore) metadata_store = NULL;
    g_autoptr (WyreboxJournalReader) reader = NULL;
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxDeliveryMaterializer) materializer = NULL;

    metadata_store = wyrebox_schema_metadata_store_new_duckdb (catalog_path,
            error);
    if (metadata_store == NULL)
        return FALSE;
    reader = wyrebox_journal_reader_new (journal_root, error);
    if (reader == NULL)
        return FALSE;
    object_store = wyrebox_local_object_store_new (object_root, error);
    if (object_store == NULL)
        return FALSE;
    materializer = wyrebox_delivery_materializer_new_duckdb (catalog_path,
            error);
    if (materializer == NULL)
        return FALSE;

    return wyrebox_delivery_catchup_materialize_account_inboxes_resumed (
        metadata_store, reader, object_store, materializer, resume_after,
        prior_holds, out_report, error);
}

static guint64
account_inbox_membership_count (const gchar *catalog_path,
    const gchar *account_id)
{
    TestDuckdbFixture duckdb = { 0 };
    g_autofree gchar *sql = NULL;
    guint64 count = 0;

    sql = g_strdup_printf ("SELECT COUNT(*) FROM mailbox_memberships "
            "WHERE account_id = '%s';", account_id);
    open_duckdb_fixture (catalog_path, &duckdb);
    count = query_uint64 (duckdb.connection, sql);
    close_duckdb_fixture (&duckdb);

    return count;
}

static guint64
account_inbox_uidnext (const gchar *catalog_path, const gchar *account_id)
{
    TestDuckdbFixture duckdb = { 0 };
    g_autofree gchar *sql = NULL;
    guint64 uidnext = 0;

    sql = g_strdup_printf ("SELECT s.uidnext FROM mailbox_uid_state s "
            "JOIN mailboxes b ON b.mailbox_id = s.namespace_id "
            "WHERE b.account_id = '%s' AND b.imap_name = 'INBOX' "
            "AND s.namespace_kind = 'mailbox';", account_id);
    open_duckdb_fixture (catalog_path, &duckdb);
    uidnext = query_uint64 (duckdb.connection, sql);
    close_duckdb_fixture (&duckdb);

    return uidnext;
}

static void
assert_catalog_checkpoint (const gchar *catalog_path,
    const WyreboxEmlIngestResult *result)
{
    TestDuckdbFixture duckdb = { 0 };

    open_duckdb_fixture (catalog_path, &duckdb);
    assert_materialization_checkpoint (duckdb.connection,
        result->journal_offset, result->journal_sequence);
    close_duckdb_fixture (&duckdb);
}

static void
assert_single_hold (const WyreboxDeliveryCatchupReport *report,
    const gchar *account_id, const WyreboxEmlIngestResult *first_held)
{
    const WyreboxDeliveryCatchupHold *hold = NULL;

    g_assert_nonnull (report->holds);
    g_assert_cmpuint (report->holds->len, ==, 1);
    hold = g_ptr_array_index (report->holds, 0);
    g_assert_cmpstr (hold->account_id, ==, account_id);
    g_assert_cmpuint (hold->journal_offset, ==, first_held->journal_offset);
    g_assert_cmpuint (hold->journal_sequence, ==,
        first_held->journal_sequence);
    g_assert_error (hold->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
}

static void
assert_materialized_accounts (const WyreboxDeliveryCatchupReport *report,
    ...)
{
    va_list args;
    guint count = 0;

    g_assert_nonnull (report->materialized_accounts);
    va_start (args, report);
    for (const gchar *account = va_arg (args, const gchar *); account != NULL;
        account = va_arg (args, const gchar *)) {
        g_assert_cmpuint (count, <, report->materialized_accounts->len);
        g_assert_cmpstr (g_ptr_array_index (report->materialized_accounts,
            count), ==, account);
        count++;
    }
    va_end (args);
    g_assert_cmpuint (report->materialized_accounts->len, ==, count);
}

typedef struct
{
    gchar *object_root;
    gchar *journal_root;
    gchar *catalog_path;
    WyreboxLocalObjectStore *object_store;
    WyreboxJournalWriter *writer;
    WyreboxEmlIngestor *ingestor;
    WyreboxEmlIngestResult a1;
    WyreboxEmlIngestResult b1;
    WyreboxEmlIngestResult a2;
    WyreboxEmlIngestResult b2;
    WyreboxEmlIngestResult a3;
} InterleavedFixture;

/*
 * Journal A1, B1, A2, B2, A3 with account-b's existing INBOX row unselectable.
 */
static void
interleaved_fixture_set_up (InterleavedFixture *fixture)
{
    fixture->object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    fixture->journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    fixture->catalog_path = create_bootstrap_catalog ();
    seed_account_b_inbox (fixture->catalog_path, FALSE);

    fixture->ingestor = create_ingestor (fixture->object_root,
            fixture->journal_root, &fixture->object_store, &fixture->writer);
    ingest_delivery_fixture (fixture->ingestor, "simple-crlf.eml",
        "delivery-a1", "account-a", &fixture->a1);
    ingest_delivery_fixture (fixture->ingestor, "duplicate-message-id.eml",
        "delivery-b1", "account-b", &fixture->b1);
    ingest_delivery_fixture (fixture->ingestor, "html-message.eml",
        "delivery-a2", "account-a", &fixture->a2);
    ingest_delivery_fixture (fixture->ingestor, "simple-crlf.eml",
        "delivery-b2", "account-b", &fixture->b2);
    ingest_delivery_fixture (fixture->ingestor, "duplicate-message-id.eml",
        "delivery-a3", "account-a", &fixture->a3);
}

static void
interleaved_fixture_tear_down (InterleavedFixture *fixture)
{
    wyrebox_eml_ingest_result_clear (&fixture->a1);
    wyrebox_eml_ingest_result_clear (&fixture->b1);
    wyrebox_eml_ingest_result_clear (&fixture->a2);
    wyrebox_eml_ingest_result_clear (&fixture->b2);
    wyrebox_eml_ingest_result_clear (&fixture->a3);
    g_clear_object (&fixture->ingestor);
    g_clear_object (&fixture->writer);
    g_clear_object (&fixture->object_store);
    remove_tree (fixture->object_root);
    remove_tree (fixture->journal_root);
    remove_catalog (fixture->catalog_path);
    g_clear_pointer (&fixture->object_root, g_free);
    g_clear_pointer (&fixture->journal_root, g_free);
    g_clear_pointer (&fixture->catalog_path, g_free);
}

static void
assert_account_a_materialized (const InterleavedFixture *fixture,
    const gchar *catalog_path)
{
    g_assert_cmpuint (account_inbox_uid (catalog_path, "account-a",
        &fixture->a1), ==, 1);
    g_assert_cmpuint (account_inbox_uid (catalog_path, "account-a",
        &fixture->a2), ==, 2);
    g_assert_cmpuint (account_inbox_uid (catalog_path, "account-a",
        &fixture->a3), ==, 3);
    g_assert_cmpuint (account_inbox_uidnext (catalog_path, "account-a"), ==,
        4);
}

static void
test_account_catchup_holds_unselectable_account (InterleavedFixture *fixture,
    gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) report = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &report, &error));
    g_assert_no_error (error);

    assert_single_hold (&report, "account-b", &fixture->b1);
    assert_account_a_materialized (fixture, fixture->catalog_path);
    g_assert_cmpuint (account_inbox_membership_count (fixture->catalog_path,
        "account-b"), ==, 0);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a1);
}

static void
test_account_catchup_held_pass_is_idempotent (InterleavedFixture *fixture,
    gconstpointer user_data)
{
    g_autoptr (GError) error = NULL;

    for (guint pass = 0; pass < 2; pass++) {
        g_auto (WyreboxDeliveryCatchupReport) report = { 0 };

        g_assert_true (run_isolated_catchup (fixture->catalog_path,
            fixture->object_root, fixture->journal_root, &report, &error));
        g_assert_no_error (error);
        assert_single_hold (&report, "account-b", &fixture->b1);
    }

    assert_account_a_materialized (fixture, fixture->catalog_path);
    g_assert_cmpuint (account_inbox_membership_count (fixture->catalog_path,
        "account-b"), ==, 0);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a1);
}

static void
test_account_catchup_held_account_recovers_like_rebuild (InterleavedFixture
    *fixture, gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) held = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) recovered = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) rebuilt = { 0 };
    g_autofree gchar *rebuilt_path = create_bootstrap_catalog ();
    g_autoptr (GError) error = NULL;
    const WyreboxEmlIngestResult *all[] = {
        &fixture->a1, &fixture->b1, &fixture->a2, &fixture->b2, &fixture->a3,
    };
    const gchar *owners[] = {
        "account-a", "account-b", "account-a", "account-b", "account-a",
    };

    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &held, &error));
    g_assert_no_error (error);
    assert_single_hold (&held, "account-b", &fixture->b1);

    execute_catalog_sql (fixture->catalog_path,
        "UPDATE mailboxes SET is_selectable = TRUE "
        "WHERE mailbox_id = 'inbox-b';");
    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &recovered, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (recovered.holds->len, ==, 0);

    assert_account_a_materialized (fixture, fixture->catalog_path);
    g_assert_cmpuint (account_inbox_uid (fixture->catalog_path, "account-b",
        &fixture->b1), ==, 1);
    g_assert_cmpuint (account_inbox_uid (fixture->catalog_path, "account-b",
        &fixture->b2), ==, 2);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a3);

    seed_account_b_inbox (rebuilt_path, TRUE);
    g_assert_true (run_isolated_catchup (rebuilt_path, fixture->object_root,
        fixture->journal_root, &rebuilt, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (rebuilt.holds->len, ==, 0);

    for (guint i = 0; i < G_N_ELEMENTS (all); i++) {
        g_assert_cmpuint (account_inbox_uid (rebuilt_path, owners[i], all[i]),
            ==, account_inbox_uid (fixture->catalog_path, owners[i], all[i]));
    }
    g_assert_cmpuint (account_inbox_uidnext (rebuilt_path, "account-a"), ==,
        account_inbox_uidnext (fixture->catalog_path, "account-a"));
    g_assert_cmpuint (account_inbox_uidnext (rebuilt_path, "account-b"), ==,
        account_inbox_uidnext (fixture->catalog_path, "account-b"));
    assert_catalog_checkpoint (rebuilt_path, &fixture->a3);

    remove_catalog (rebuilt_path);
}

static void
test_account_catchup_without_report_fails_on_hold (InterleavedFixture
    *fixture, gconstpointer user_data)
{
    g_autoptr (GError) error = NULL;

    g_assert_false (run_account_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, "inbox-b"));
    assert_account_a_materialized (fixture, fixture->catalog_path);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a1);
}

static void
assert_scanned_through (const WyreboxDeliveryCatchupReport *report,
    const WyreboxEmlIngestResult *last)
{
    g_assert_true (report->scanned_through.present);
    g_assert_cmpuint (report->scanned_through.journal_offset, ==,
        last->journal_offset);
    g_assert_cmpuint (report->scanned_through.journal_sequence, ==,
        last->journal_sequence);
}

static void
test_account_catchup_resume_skips_scanned_backlog (InterleavedFixture
    *fixture, gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) full = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) resumed = { 0 };
    g_auto (WyreboxEmlIngestResult) a4 = { 0 };
    g_auto (WyreboxEmlIngestResult) b3 = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &full, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (full.records_scanned, ==, 5);
    assert_scanned_through (&full, &fixture->a3);
    assert_single_hold (&full, "account-b", &fixture->b1);

    make_account_b_inbox_selectable_in (fixture->catalog_path);
    ingest_delivery_fixture (fixture->ingestor, "html-message.eml",
        "delivery-a4", "account-a", &a4);
    ingest_delivery_fixture (fixture->ingestor, "simple-crlf.eml",
        "delivery-b3", "account-b", &b3);

    g_assert_true (run_resumed_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &full.scanned_through,
        full.holds, &resumed, &error));
    g_assert_no_error (error);

    g_assert_cmpuint (resumed.records_scanned, ==, 2);
    assert_scanned_through (&resumed, &b3);
    assert_single_hold (&resumed, "account-b", &fixture->b1);
    g_assert_cmpuint (account_inbox_uid (fixture->catalog_path, "account-a",
        &a4), ==, 4);
    g_assert_cmpuint (account_inbox_membership_count (fixture->catalog_path,
        "account-b"), ==, 0);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a1);
}

static void
test_account_catchup_resume_past_checkpoint_keeps_checkpoint (
    InterleavedFixture *fixture, gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) full = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) resumed = { 0 };
    g_auto (WyreboxEmlIngestResult) a4 = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &full, &error));
    g_assert_no_error (error);
    ingest_delivery_fixture (fixture->ingestor, "html-message.eml",
        "delivery-a4", "account-a", &a4);

    g_assert_true (run_resumed_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &full.scanned_through,
        NULL, &resumed, &error));
    g_assert_no_error (error);

    g_assert_cmpuint (resumed.records_scanned, ==, 1);
    g_assert_cmpuint (resumed.holds->len, ==, 0);
    g_assert_cmpuint (account_inbox_uid (fixture->catalog_path, "account-a",
        &a4), ==, 4);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a1);
}

static void
test_account_catchup_resume_without_new_records_keeps_cursor (
    InterleavedFixture *fixture, gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) full = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) resumed = { 0 };
    g_autoptr (GError) error = NULL;

    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &full, &error));
    g_assert_no_error (error);

    g_assert_true (run_resumed_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &full.scanned_through,
        full.holds, &resumed, &error));
    g_assert_no_error (error);

    g_assert_cmpuint (resumed.records_scanned, ==, 0);
    assert_scanned_through (&resumed, &fixture->a3);
    assert_single_hold (&resumed, "account-b", &fixture->b1);
}

static void
test_account_catchup_resume_adds_new_hold_after_prior (InterleavedFixture
    *fixture, gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) full = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) resumed = { 0 };
    g_auto (WyreboxEmlIngestResult) c1 = { 0 };
    g_autoptr (GError) error = NULL;
    const WyreboxDeliveryCatchupHold *hold = NULL;

    execute_catalog_sql (fixture->catalog_path,
        "INSERT INTO accounts (account_id) VALUES ('account-c');");
    execute_catalog_sql (fixture->catalog_path,
        "INSERT INTO mailboxes (mailbox_id, account_id, imap_name, "
        "is_selectable, is_visible) VALUES "
        "('inbox-c', 'account-c', 'INBOX', FALSE, TRUE);");
    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &full, &error));
    g_assert_no_error (error);
    ingest_delivery_fixture (fixture->ingestor, "html-message.eml",
        "delivery-c1", "account-c", &c1);

    g_assert_true (run_resumed_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &full.scanned_through,
        full.holds, &resumed, &error));
    g_assert_no_error (error);

    g_assert_cmpuint (resumed.holds->len, ==, 2);
    hold = g_ptr_array_index (resumed.holds, 0);
    g_assert_cmpstr (hold->account_id, ==, "account-b");
    g_assert_cmpuint (hold->journal_offset, ==, fixture->b1.journal_offset);
    hold = g_ptr_array_index (resumed.holds, 1);
    g_assert_cmpstr (hold->account_id, ==, "account-c");
    g_assert_cmpuint (hold->journal_offset, ==, c1.journal_offset);
    g_assert_error (hold->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a1);
}

static void
test_account_catchup_stale_cursor_aborts (InterleavedFixture *fixture,
    gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) report = { 0 };
    g_autoptr (GError) error = NULL;
    WyreboxDeliveryCatchupCursor stale = {
        .present = TRUE,
        .journal_offset = fixture->a3.journal_offset,
        .journal_sequence = fixture->a3.journal_sequence + 1,
    };

    g_assert_false (run_resumed_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &stale, NULL, &report,
        &error));
    g_assert_nonnull (error);
    g_assert_true (g_str_has_prefix (error->message, "scan cursor: "));
    g_assert_null (report.holds);
}

static void
test_account_catchup_resume_at_checkpoint_advances (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) a1 = { 0 };
    g_auto (WyreboxEmlIngestResult) a2 = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) full = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) resumed = { 0 };
    g_autoptr (GError) error = NULL;

    ingestor = create_ingestor (object_root, journal_root, &object_store,
            &writer);
    ingest_delivery_fixture (ingestor, "simple-crlf.eml", "delivery-a1",
        "account-a", &a1);
    g_assert_true (run_isolated_catchup (catalog_path, object_root,
        journal_root, &full, &error));
    g_assert_no_error (error);
    assert_scanned_through (&full, &a1);
    assert_catalog_checkpoint (catalog_path, &a1);

    ingest_delivery_fixture (ingestor, "html-message.eml", "delivery-a2",
        "account-a", &a2);
    g_assert_true (run_resumed_catchup (catalog_path, object_root,
        journal_root, &full.scanned_through, NULL, &resumed, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (resumed.records_scanned, ==, 1);
    assert_catalog_checkpoint (catalog_path, &a2);

    g_clear_object (&ingestor);
    g_clear_object (&writer);
    g_clear_object (&object_store);
    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

static void
test_account_catchup_hold_on_first_record_keeps_no_checkpoint (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) b1 = { 0 };
    g_auto (WyreboxEmlIngestResult) a1 = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) report = { 0 };
    g_autoptr (GError) error = NULL;
    TestDuckdbFixture duckdb = { 0 };

    seed_account_b_inbox (catalog_path, FALSE);
    ingestor = create_ingestor (object_root, journal_root, &object_store,
            &writer);
    ingest_delivery_fixture (ingestor, "simple-crlf.eml", "delivery-b1",
        "account-b", &b1);
    ingest_delivery_fixture (ingestor, "html-message.eml", "delivery-a1",
        "account-a", &a1);

    g_assert_true (run_isolated_catchup (catalog_path, object_root,
        journal_root, &report, &error));
    g_assert_no_error (error);
    assert_single_hold (&report, "account-b", &b1);
    g_assert_cmpuint (account_inbox_uid (catalog_path, "account-a", &a1), ==,
        1);

    open_duckdb_fixture (catalog_path, &duckdb);
    assert_table_count (duckdb.connection, "materialization_checkpoint", 0);
    close_duckdb_fixture (&duckdb);

    g_clear_object (&ingestor);
    g_clear_object (&writer);
    g_clear_object (&object_store);
    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

/*
 * Journal A1, B1, A2, B2, A3, where account-b's raw objects are not shared
 * with account-a.
 */
static void
object_fixture_set_up (InterleavedFixture *fixture)
{
    fixture->object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    fixture->journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    fixture->catalog_path = create_bootstrap_catalog ();

    fixture->ingestor = create_ingestor (fixture->object_root,
            fixture->journal_root, &fixture->object_store, &fixture->writer);
    ingest_delivery_fixture (fixture->ingestor, "simple-crlf.eml",
        "delivery-a1", "account-a", &fixture->a1);
    ingest_delivery_fixture (fixture->ingestor, "folded-subject.eml",
        "delivery-b1", "account-b", &fixture->b1);
    ingest_delivery_fixture (fixture->ingestor, "html-message.eml",
        "delivery-a2", "account-a", &fixture->a2);
    ingest_delivery_fixture (fixture->ingestor, "non-ascii-headers.eml",
        "delivery-b2", "account-b", &fixture->b2);
    ingest_delivery_fixture (fixture->ingestor, "duplicate-message-id.eml",
        "delivery-a3", "account-a", &fixture->a3);
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

static GBytes *
remove_raw_object (const gchar *object_root, const gchar *object_key)
{
    g_autofree gchar *path = raw_object_path (object_root, object_key);
    g_autoptr (GError) error = NULL;
    gchar *contents = NULL;
    gsize length = 0;

    g_assert_true (g_file_get_contents (path, &contents, &length, &error));
    g_assert_no_error (error);
    g_assert_cmpint (g_remove (path), ==, 0);

    return g_bytes_new_take (contents, length);
}

static void
restore_raw_object (const gchar *object_root, const gchar *object_key,
    GBytes *bytes)
{
    g_autofree gchar *path = raw_object_path (object_root, object_key);
    g_autoptr (GError) error = NULL;
    gsize length = 0;
    const gchar *data = g_bytes_get_data (bytes, &length);

    g_assert_true (g_file_set_contents (path, data, (gssize)length, &error));
    g_assert_no_error (error);
}

static void
assert_hold_mentions_object (const WyreboxDeliveryCatchupReport *report,
    const WyreboxEmlIngestResult *held)
{
    const WyreboxDeliveryCatchupHold *hold =
        g_ptr_array_index (report->holds, 0);
    g_autofree gchar *sequence =
        g_strdup_printf ("sequence %" G_GUINT64_FORMAT,
            held->journal_sequence);

    g_assert_nonnull (strstr (hold->error->message, sequence));
    g_assert_nonnull (strstr (hold->error->message, held->object_key));
}

static void
assert_account_b_held_at_b1 (const InterleavedFixture *fixture,
    const WyreboxDeliveryCatchupReport *report)
{
    assert_single_hold (report, "account-b", &fixture->b1);
    assert_account_a_materialized (fixture, fixture->catalog_path);
    g_assert_cmpuint (account_inbox_membership_count (fixture->catalog_path,
        "account-b"), ==, 0);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a1);
}

static void
test_account_catchup_missing_object_holds_account (InterleavedFixture
    *fixture, gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) report = { 0 };
    g_autoptr (GBytes) removed = NULL;
    g_autoptr (GError) error = NULL;

    removed = remove_raw_object (fixture->object_root, fixture->b1.object_key);

    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &report, &error));
    g_assert_no_error (error);

    assert_account_b_held_at_b1 (fixture, &report);
    assert_hold_mentions_object (&report, &fixture->b1);
}

static void
test_account_catchup_hash_mismatch_holds_account (InterleavedFixture
    *fixture, gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) report = { 0 };
    g_autoptr (GBytes) original = NULL;
    g_autoptr (GBytes) tampered = NULL;
    g_autoptr (GError) error = NULL;
    g_autofree guint8 *data = NULL;
    gsize length = 0;

    original = remove_raw_object (fixture->object_root,
            fixture->b1.object_key);
    data = g_bytes_unref_to_data (g_steal_pointer (&original), &length);
    g_assert_cmpuint (length, >, 0);
    data[length - 1] ^= 0x01;
    tampered = g_bytes_new (data, length);
    restore_raw_object (fixture->object_root, fixture->b1.object_key,
        tampered);

    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &report, &error));
    g_assert_no_error (error);

    assert_account_b_held_at_b1 (fixture, &report);
    assert_hold_mentions_object (&report, &fixture->b1);
    g_assert_nonnull (strstr (((WyreboxDeliveryCatchupHold *)
        g_ptr_array_index (report.holds, 0))->error->message, "SHA-256"));
}

static void
test_account_catchup_size_mismatch_holds_account (InterleavedFixture
    *fixture, gconstpointer user_data)
{
    const gchar *const recipients[] = { "user@example.test", NULL };
    g_auto (WyreboxDeliveryCatchupReport) report = { 0 };
    g_autoptr (GBytes) payload = NULL;
    g_autoptr (GError) error = NULL;
    WyreboxEmlIngestResult b3 = { 0 };
    const WyreboxDeliveryCatchupHold *hold = NULL;

    payload = wyrebox_message_delivered_payload_encode_with_identity (
        fixture->b1.object_key, fixture->b1.size_bytes - 1, NULL, 0,
        "delivery-b3", NULL, "account-b", "sender@example.test", recipients,
        &error);
    g_assert_no_error (error);
    g_assert_true (wyrebox_journal_writer_append (fixture->writer,
        WYREBOX_JOURNAL_EVENT_MESSAGE_DELIVERED, payload, &b3.journal_offset,
        &b3.journal_sequence, &error));
    g_assert_no_error (error);

    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &report, &error));
    g_assert_no_error (error);

    assert_single_hold (&report, "account-b", &b3);
    hold = g_ptr_array_index (report.holds, 0);
    g_assert_nonnull (strstr (hold->error->message, "mismatched size"));
    assert_account_a_materialized (fixture, fixture->catalog_path);
    g_assert_cmpuint (account_inbox_membership_count (fixture->catalog_path,
        "account-b"), ==, 2);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a3);
}

static void
test_account_catchup_unreadable_object_holds_account (InterleavedFixture
    *fixture, gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) report = { 0 };
    g_autoptr (GBytes) removed = NULL;
    g_autoptr (GError) error = NULL;
    g_autofree gchar *path = raw_object_path (fixture->object_root,
            fixture->b1.object_key);
    const WyreboxDeliveryCatchupHold *hold = NULL;

    removed = remove_raw_object (fixture->object_root, fixture->b1.object_key);
    g_assert_cmpint (g_mkdir (path, 0700), ==, 0);

    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &report, &error));
    g_assert_no_error (error);

    g_assert_cmpuint (report.holds->len, ==, 1);
    hold = g_ptr_array_index (report.holds, 0);
    g_assert_cmpstr (hold->account_id, ==, "account-b");
    g_assert_cmpuint (hold->journal_sequence, ==, fixture->b1.journal_sequence);
    g_assert_error (hold->error, G_FILE_ERROR, G_FILE_ERROR_ISDIR);
    assert_account_a_materialized (fixture, fixture->catalog_path);
    g_assert_cmpuint (account_inbox_membership_count (fixture->catalog_path,
        "account-b"), ==, 0);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a1);

    g_assert_cmpint (g_rmdir (path), ==, 0);
}

static void
test_account_catchup_restored_object_recovers (InterleavedFixture *fixture,
    gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) held = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) recovered = { 0 };
    g_autoptr (GBytes) removed = NULL;
    g_autoptr (GError) error = NULL;

    removed = remove_raw_object (fixture->object_root, fixture->b1.object_key);
    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &held, &error));
    g_assert_no_error (error);
    assert_account_b_held_at_b1 (fixture, &held);

    restore_raw_object (fixture->object_root, fixture->b1.object_key, removed);
    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &recovered, &error));
    g_assert_no_error (error);

    g_assert_cmpuint (recovered.holds->len, ==, 0);
    assert_account_a_materialized (fixture, fixture->catalog_path);
    g_assert_cmpuint (account_inbox_uid (fixture->catalog_path, "account-b",
        &fixture->b1), ==, 1);
    g_assert_cmpuint (account_inbox_uid (fixture->catalog_path, "account-b",
        &fixture->b2), ==, 2);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a3);
}

static void
test_account_catchup_resume_holds_new_missing_object (InterleavedFixture
    *fixture, gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) full = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) resumed = { 0 };
    g_auto (WyreboxEmlIngestResult) b3 = { 0 };
    g_auto (WyreboxEmlIngestResult) a4 = { 0 };
    g_autoptr (GBytes) removed = NULL;
    g_autoptr (GError) error = NULL;

    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &full, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (full.holds->len, ==, 0);

    ingest_delivery_fixture (fixture->ingestor, "multipart-attachment-like.eml",
        "delivery-b3", "account-b", &b3);
    ingest_delivery_fixture (fixture->ingestor, "html-message.eml",
        "delivery-a4", "account-a", &a4);
    removed = remove_raw_object (fixture->object_root, b3.object_key);

    g_assert_true (run_resumed_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &full.scanned_through,
        full.holds, &resumed, &error));
    g_assert_no_error (error);

    assert_single_hold (&resumed, "account-b", &b3);
    assert_hold_mentions_object (&resumed, &b3);
    g_assert_cmpuint (account_inbox_uid (fixture->catalog_path, "account-a",
        &a4), ==, 4);
    g_assert_cmpuint (account_inbox_membership_count (fixture->catalog_path,
        "account-b"), ==, 2);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a3);
}

static void
test_account_catchup_without_report_fails_on_missing_object (
    InterleavedFixture *fixture, gconstpointer user_data)
{
    g_autoptr (GBytes) removed = NULL;
    g_autoptr (GError) error = NULL;

    removed = remove_raw_object (fixture->object_root, fixture->b1.object_key);

    g_assert_false (run_account_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, fixture->b1.object_key));
    assert_account_a_materialized (fixture, fixture->catalog_path);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a1);
}

static void
test_account_catchup_missing_object_root_aborts_pass (InterleavedFixture
    *fixture, gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) report = { 0 };
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxSchemaMetadataStore) metadata_store = NULL;
    g_autoptr (WyreboxJournalReader) reader = NULL;
    g_autoptr (WyreboxDeliveryMaterializer) materializer = NULL;
    g_autofree gchar *objects_dir = g_build_filename (fixture->object_root,
            "objects", "sha256", NULL);

    metadata_store = wyrebox_schema_metadata_store_new_duckdb (
        fixture->catalog_path, &error);
    g_assert_no_error (error);
    reader = wyrebox_journal_reader_new (fixture->journal_root, &error);
    g_assert_no_error (error);
    materializer = wyrebox_delivery_materializer_new_duckdb (
        fixture->catalog_path, &error);
    g_assert_no_error (error);
    remove_tree (objects_dir);

    g_assert_false (
        wyrebox_delivery_catchup_materialize_account_inboxes_isolated (
            metadata_store, reader, fixture->object_store, materializer,
            &report, &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_object (&materializer);
    g_clear_object (&metadata_store);
    g_assert_null (report.holds);
    g_assert_false (g_file_test (objects_dir, G_FILE_TEST_EXISTS));
    assert_unmaterialized_state (fixture->catalog_path);
}

static void
test_account_catchup_shared_missing_object_holds_both_accounts (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) a1 = { 0 };
    g_auto (WyreboxEmlIngestResult) b1 = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) held = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) recovered = { 0 };
    g_autoptr (GBytes) removed = NULL;
    g_autoptr (GError) error = NULL;
    const WyreboxDeliveryCatchupHold *hold = NULL;

    ingestor = create_ingestor (object_root, journal_root, &object_store,
            &writer);
    ingest_delivery_fixture (ingestor, "simple-crlf.eml", "delivery-a1",
        "account-a", &a1);
    ingest_delivery_fixture (ingestor, "simple-crlf.eml", "delivery-b1",
        "account-b", &b1);
    g_assert_cmpstr (a1.object_key, ==, b1.object_key);
    removed = remove_raw_object (object_root, a1.object_key);

    g_assert_true (run_isolated_catchup (catalog_path, object_root,
        journal_root, &held, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (held.holds->len, ==, 2);
    hold = g_ptr_array_index (held.holds, 0);
    g_assert_cmpstr (hold->account_id, ==, "account-a");
    g_assert_cmpuint (hold->journal_sequence, ==, a1.journal_sequence);
    g_assert_error (hold->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    hold = g_ptr_array_index (held.holds, 1);
    g_assert_cmpstr (hold->account_id, ==, "account-b");
    g_assert_cmpuint (hold->journal_sequence, ==, b1.journal_sequence);
    g_assert_error (hold->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_cmpuint (account_inbox_membership_count (catalog_path,
        "account-a"), ==, 0);
    g_assert_cmpuint (account_inbox_membership_count (catalog_path,
        "account-b"), ==, 0);

    restore_raw_object (object_root, a1.object_key, removed);
    g_assert_true (run_isolated_catchup (catalog_path, object_root,
        journal_root, &recovered, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (recovered.holds->len, ==, 0);
    g_assert_cmpuint (account_inbox_uid (catalog_path, "account-a", &a1), ==,
        1);
    g_assert_cmpuint (account_inbox_uid (catalog_path, "account-b", &b1), ==,
        1);
    assert_catalog_checkpoint (catalog_path, &b1);

    g_clear_object (&ingestor);
    g_clear_object (&writer);
    g_clear_object (&object_store);
    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

static void
test_account_catchup_missing_object_mid_run_holds_whole_run (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) a1 = { 0 };
    g_auto (WyreboxEmlIngestResult) b1 = { 0 };
    g_auto (WyreboxEmlIngestResult) b2 = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) report = { 0 };
    g_autoptr (GBytes) removed = NULL;
    g_autoptr (GError) error = NULL;

    ingestor = create_ingestor (object_root, journal_root, &object_store,
            &writer);
    ingest_delivery_fixture (ingestor, "simple-crlf.eml", "delivery-a1",
        "account-a", &a1);
    ingest_delivery_fixture (ingestor, "folded-subject.eml", "delivery-b1",
        "account-b", &b1);
    ingest_delivery_fixture (ingestor, "non-ascii-headers.eml", "delivery-b2",
        "account-b", &b2);
    removed = remove_raw_object (object_root, b2.object_key);

    g_assert_true (run_isolated_catchup (catalog_path, object_root,
        journal_root, &report, &error));
    g_assert_no_error (error);

    assert_single_hold (&report, "account-b", &b1);
    assert_hold_mentions_object (&report, &b2);
    g_assert_cmpuint (account_inbox_uid (catalog_path, "account-a", &a1), ==,
        1);
    g_assert_cmpuint (account_inbox_membership_count (catalog_path,
        "account-b"), ==, 0);
    assert_catalog_checkpoint (catalog_path, &a1);

    g_clear_object (&ingestor);
    g_clear_object (&writer);
    g_clear_object (&object_store);
    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

static void
test_missing_object_fails_single_inbox_catchup (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) first = { 0 };
    g_auto (WyreboxEmlIngestResult) second = { 0 };
    g_autoptr (GBytes) removed = NULL;
    g_autoptr (GError) error = NULL;

    ingestor = create_ingestor (object_root, journal_root, &object_store,
            &writer);
    ingest_fixture (ingestor, "simple-crlf.eml", &first);
    ingest_fixture (ingestor, "html-message.eml", &second);
    removed = remove_raw_object (object_root, second.object_key);

    g_assert_false (run_catchup (catalog_path, object_root, journal_root,
        &error));
    g_assert_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull (strstr (error->message, second.object_key));
    assert_unmaterialized_state (catalog_path);

    g_clear_object (&ingestor);
    g_clear_object (&writer);
    g_clear_object (&object_store);
    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

typedef struct
{
    guint64 journal_offset;
    guint64 journal_sequence;
} JournalPosition;

static JournalPosition
append_flag_change (WyreboxJournalWriter *writer, const gchar *account_id,
    const gchar *mailbox_id, guint64 uid, WyreboxFlagChangedMode mode,
    const gchar *flags, const gchar *keywords)
{
    g_auto (GStrv) system_flags = g_strsplit (flags, " ", -1);
    g_auto (GStrv) user_keywords = g_strsplit (keywords, " ", -1);
    WyreboxFlagChangedPayload payload = {
        .account_id = (char *)account_id,
        .mailbox_id = (char *)mailbox_id,
        .uidvalidity = 1,
        .uid = uid,
        .mode = mode,
        .system_flags = flags[0] != '\0' ? system_flags : NULL,
        .user_keywords = keywords[0] != '\0' ? user_keywords : NULL,
    };
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) bytes = NULL;
    JournalPosition position = { 0 };

    bytes = wyrebox_flag_changed_payload_encode (&payload, &error);
    g_assert_no_error (error);
    g_assert_true (wyrebox_journal_writer_append (writer,
        WYREBOX_JOURNAL_EVENT_FLAG_CHANGED, bytes, &position.journal_offset,
        &position.journal_sequence, &error));
    g_assert_no_error (error);
    return position;
}

static gchar *
flag_rows (const gchar *catalog_path)
{
    TestDuckdbFixture duckdb = { 0 };
    g_autofree gchar *flags = NULL;
    g_autofree gchar *keywords = NULL;

    open_duckdb_fixture (catalog_path, &duckdb);
    flags = query_string (duckdb.connection,
            "SELECT COALESCE(string_agg(mm.account_id || ':' || mm.uid || ' ' || "
            "f.flag_name || '@' || f.journal_offset, ', ' "
            "ORDER BY mm.account_id, mm.uid, f.flag_name), '') "
            "FROM message_flags f JOIN mailbox_memberships mm "
            "ON mm.membership_id = f.membership_id;");
    keywords = query_string (duckdb.connection,
            "SELECT COALESCE(string_agg(mm.account_id || ':' || mm.uid || ' ' || "
            "k.keyword_name || '@' || k.journal_offset, ', ' "
            "ORDER BY mm.account_id, mm.uid, k.keyword_name), '') "
            "FROM message_keywords k JOIN mailbox_memberships mm "
            "ON mm.membership_id = k.membership_id;");
    close_duckdb_fixture (&duckdb);

    return g_strdup_printf ("flags [%s] keywords [%s]", flags, keywords);
}

static void
assert_catalog_checkpoint_at (const gchar *catalog_path,
    JournalPosition position)
{
    TestDuckdbFixture duckdb = { 0 };

    open_duckdb_fixture (catalog_path, &duckdb);
    assert_materialization_checkpoint (duckdb.connection,
        position.journal_offset, position.journal_sequence);
    close_duckdb_fixture (&duckdb);
}

static void
test_account_catchup_flag_changes_follow_holds (InterleavedFixture *fixture,
    gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) held = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) recovered = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) rebuilt = { 0 };
    g_autofree gchar *rebuilt_path = create_bootstrap_catalog ();
    g_autofree gchar *expected = NULL;
    g_autofree gchar *held_rows = NULL;
    g_autofree gchar *recovered_rows = NULL;
    g_autofree gchar *rebuilt_rows = NULL;
    g_autoptr (GError) error = NULL;
    JournalPosition seen = { 0 };
    JournalPosition flagged = { 0 };
    JournalPosition answered = { 0 };

    seen = append_flag_change (fixture->writer, "account-a",
            "inbox:account-a", 2, WYREBOX_FLAG_CHANGED_MODE_SET, "\\Seen",
            "work");
    flagged = append_flag_change (fixture->writer, "account-b", "inbox-b", 1,
            WYREBOX_FLAG_CHANGED_MODE_SET, "\\Flagged", "");
    answered = append_flag_change (fixture->writer, "account-a",
            "inbox:account-a", 1, WYREBOX_FLAG_CHANGED_MODE_REPLACE,
            "\\Answered",
            "");

    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &held, &error));
    g_assert_no_error (error);
    assert_single_hold (&held, "account-b", &fixture->b1);
    g_assert_cmpuint (held.records_scanned, ==, 8);
    g_assert_cmpuint (held.scanned_through.journal_sequence, ==,
        answered.journal_sequence);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a1);
    held_rows = flag_rows (fixture->catalog_path);
    expected = g_strdup_printf ("flags [account-a:1 \\Answered@%"
            G_GUINT64_FORMAT ", account-a:2 \\Seen@%" G_GUINT64_FORMAT
            "] keywords [account-a:2 work@%" G_GUINT64_FORMAT "]",
            answered.journal_offset, seen.journal_offset, seen.journal_offset);
    g_assert_cmpstr (held_rows, ==, expected);

    make_account_b_inbox_selectable_in (fixture->catalog_path);
    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &recovered, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (recovered.holds->len, ==, 0);
    assert_catalog_checkpoint_at (fixture->catalog_path, answered);
    recovered_rows = flag_rows (fixture->catalog_path);
    g_clear_pointer (&expected, g_free);
    expected = g_strdup_printf ("flags [account-a:1 \\Answered@%"
            G_GUINT64_FORMAT ", account-a:2 \\Seen@%" G_GUINT64_FORMAT
            ", account-b:1 \\Flagged@%" G_GUINT64_FORMAT
            "] keywords [account-a:2 work@%" G_GUINT64_FORMAT "]",
            answered.journal_offset, seen.journal_offset,
            flagged.journal_offset,
            seen.journal_offset);
    g_assert_cmpstr (recovered_rows, ==, expected);

    seed_account_b_inbox (rebuilt_path, TRUE);
    g_assert_true (run_isolated_catchup (rebuilt_path, fixture->object_root,
        fixture->journal_root, &rebuilt, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (rebuilt.holds->len, ==, 0);
    assert_catalog_checkpoint_at (rebuilt_path, answered);
    rebuilt_rows = flag_rows (rebuilt_path);
    g_assert_cmpstr (rebuilt_rows, ==, recovered_rows);

    remove_catalog (rebuilt_path);
}

static JournalPosition
append_fact_mutation (WyreboxJournalWriter *writer,
    WyreboxDaemonFactMutationKind kind, const gchar *account_id,
    const WyreboxEmlIngestResult *message, const gchar *view_id)
{
    g_autofree gchar *message_id = g_strdup_printf ("journal:%"
            G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT, message->journal_offset,
            message->journal_sequence);
    const gchar *const arguments[] = { message_id, view_id, NULL };
    g_auto (WyreboxDaemonFactMutationRequest) request = { 0 };
    g_autoptr (GError) error = NULL;
    JournalPosition position = { 0 };

    g_assert_true (wyrebox_daemon_fact_mutation_request_init (&request, kind,
        "project_keyword", account_id, arguments, &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_fact_mutation_request_append_journal
            (&request, writer, &position.journal_offset,
        &position.journal_sequence, &error));
    g_assert_no_error (error);
    return position;
}

/*
 * Summarizes message_facts as account/message/object/args/retracted rows.
 */
static gchar *
fact_rows (const gchar *catalog_path)
{
    TestDuckdbFixture duckdb = { 0 };
    gchar *rows = NULL;

    open_duckdb_fixture (catalog_path, &duckdb);
    rows = query_string (duckdb.connection,
            "SELECT COALESCE(string_agg(account_id || ' ' || message_id || "
            "' ' || object_id || ' ' || args_json || ' ' || "
            "retracted_at_unix_us || '@' || journal_sequence, '; ' "
            "ORDER BY account_id, args_json), '') FROM message_facts;");
    close_duckdb_fixture (&duckdb);

    return rows;
}

static gchar *
expected_fact_row (const gchar *account_id,
    const WyreboxEmlIngestResult *message, const gchar *view_id,
    guint64 retracted_at, JournalPosition last_change)
{
    return g_strdup_printf ("%s journal:%" G_GUINT64_FORMAT ":%"
               G_GUINT64_FORMAT " %s [\"journal:%" G_GUINT64_FORMAT ":%"
               G_GUINT64_FORMAT "\",\"%s\"] %" G_GUINT64_FORMAT "@%"
               G_GUINT64_FORMAT, account_id, message->journal_offset,
               message->journal_sequence, message->object_key,
               message->journal_offset, message->journal_sequence, view_id,
               retracted_at, last_change.journal_sequence);
}

static void
test_account_catchup_fact_mutations_follow_holds (InterleavedFixture *fixture,
    gconstpointer user_data)
{
    g_auto (WyreboxDeliveryCatchupReport) held = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) recovered = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) rebuilt = { 0 };
    g_autofree gchar *rebuilt_path = create_bootstrap_catalog ();
    g_autofree gchar *a_projects = NULL;
    g_autofree gchar *a_ops = NULL;
    g_autofree gchar *b_projects = NULL;
    g_autofree gchar *expected = NULL;
    g_autofree gchar *held_rows = NULL;
    g_autofree gchar *recovered_rows = NULL;
    g_autofree gchar *rebuilt_rows = NULL;
    g_autoptr (GError) error = NULL;
    JournalPosition a_insert = { 0 };
    JournalPosition b_insert = { 0 };
    JournalPosition a_ops_insert = { 0 };
    JournalPosition a_retract = { 0 };

    a_insert = append_fact_mutation (fixture->writer,
            WYREBOX_DAEMON_FACT_MUTATION_INSERT, "account-a", &fixture->a1,
            "view-projects");
    b_insert = append_fact_mutation (fixture->writer,
            WYREBOX_DAEMON_FACT_MUTATION_INSERT, "account-b", &fixture->b1,
            "view-projects");
    a_ops_insert = append_fact_mutation (fixture->writer,
            WYREBOX_DAEMON_FACT_MUTATION_INSERT, "account-a", &fixture->a2,
            "view-ops");
    a_retract = append_fact_mutation (fixture->writer,
            WYREBOX_DAEMON_FACT_MUTATION_RETRACT, "account-a", &fixture->a1,
            "view-projects");

    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &held, &error));
    g_assert_no_error (error);
    assert_single_hold (&held, "account-b", &fixture->b1);
    g_assert_cmpuint (held.records_scanned, ==, 9);
    assert_materialized_accounts (&held, "account-a", NULL);
    assert_catalog_checkpoint (fixture->catalog_path, &fixture->a1);

    a_projects = expected_fact_row ("account-a", &fixture->a1,
            "view-projects", a_retract.journal_sequence, a_retract);
    a_ops = expected_fact_row ("account-a", &fixture->a2, "view-ops", 0,
            a_ops_insert);
    held_rows = fact_rows (fixture->catalog_path);
    expected = g_strdup_printf ("%s; %s", a_projects, a_ops);
    g_assert_cmpstr (held_rows, ==, expected);
    g_assert_cmpuint (a_insert.journal_sequence, <,
        a_retract.journal_sequence);

    make_account_b_inbox_selectable_in (fixture->catalog_path);
    g_assert_true (run_isolated_catchup (fixture->catalog_path,
        fixture->object_root, fixture->journal_root, &recovered, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (recovered.holds->len, ==, 0);
    assert_materialized_accounts (&recovered, "account-a", "account-b", NULL);
    assert_catalog_checkpoint_at (fixture->catalog_path, a_retract);
    b_projects = expected_fact_row ("account-b", &fixture->b1,
            "view-projects", 0, b_insert);
    recovered_rows = fact_rows (fixture->catalog_path);
    g_clear_pointer (&expected, g_free);
    expected = g_strdup_printf ("%s; %s; %s", a_projects, a_ops, b_projects);
    g_assert_cmpstr (recovered_rows, ==, expected);

    seed_account_b_inbox (rebuilt_path, TRUE);
    g_assert_true (run_isolated_catchup (rebuilt_path, fixture->object_root,
        fixture->journal_root, &rebuilt, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (rebuilt.holds->len, ==, 0);
    assert_catalog_checkpoint_at (rebuilt_path, a_retract);
    rebuilt_rows = fact_rows (rebuilt_path);
    g_assert_cmpstr (rebuilt_rows, ==, recovered_rows);

    remove_catalog (rebuilt_path);
}

static void
test_account_catchup_unknown_flag_target_holds_account (void)
{
    g_autofree gchar *object_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-objects-XXXXXX", NULL);
    g_autofree gchar *journal_root =
        g_dir_make_tmp ("wyrebox-delivery-catchup-journal-XXXXXX", NULL);
    g_autofree gchar *catalog_path = create_bootstrap_catalog ();
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;
    g_auto (WyreboxEmlIngestResult) a1 = { 0 };
    g_auto (WyreboxEmlIngestResult) a2 = { 0 };
    g_auto (WyreboxEmlIngestResult) b1 = { 0 };
    g_auto (WyreboxDeliveryCatchupReport) report = { 0 };
    const WyreboxDeliveryCatchupHold *hold = NULL;
    g_autofree gchar *rows = NULL;
    JournalPosition unknown = { 0 };

    ingestor = create_ingestor (object_root, journal_root, &object_store,
            &writer);
    ingest_delivery_fixture (ingestor, "simple-crlf.eml", "delivery-a1",
        "account-a", &a1);
    unknown = append_flag_change (writer, "account-a", "inbox:account-a", 5,
            WYREBOX_FLAG_CHANGED_MODE_SET, "\\Seen", "");
    ingest_delivery_fixture (ingestor, "html-message.eml", "delivery-a2",
        "account-a", &a2);
    ingest_delivery_fixture (ingestor, "duplicate-message-id.eml",
        "delivery-b1", "account-b", &b1);

    g_assert_true (run_isolated_catchup (catalog_path, object_root,
        journal_root, &report, &error));
    g_assert_no_error (error);
    g_assert_cmpuint (report.holds->len, ==, 1);
    hold = g_ptr_array_index (report.holds, 0);
    g_assert_cmpstr (hold->account_id, ==, "account-a");
    g_assert_cmpuint (hold->journal_offset, ==, unknown.journal_offset);
    g_assert_cmpuint (hold->journal_sequence, ==, unknown.journal_sequence);
    g_assert_error (hold->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);

    g_assert_cmpuint (account_inbox_membership_count (catalog_path,
        "account-a"), ==, 1);
    g_assert_cmpuint (account_inbox_uid (catalog_path, "account-b", &b1), ==,
        1);
    assert_catalog_checkpoint (catalog_path, &a1);
    rows = flag_rows (catalog_path);
    g_assert_cmpstr (rows, ==, "flags [] keywords []");

    g_clear_object (&ingestor);
    g_clear_object (&writer);
    g_clear_object (&object_store);
    remove_tree (object_root);
    remove_tree (journal_root);
    remove_catalog (catalog_path);
}

#define ADD_OBJECT_TEST(path, func) \
        g_test_add ("/ingestion/delivery-catchup/accounts/objects/" path, \
            InterleavedFixture, NULL, \
            (void (*)(InterleavedFixture *, \
            gconstpointer)) object_fixture_set_up, \
            func, \
            (void (*)(InterleavedFixture *, gconstpointer)) \
            interleaved_fixture_tear_down)

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add (
        "/ingestion/delivery-catchup/accounts/flag-changes-follow-holds",
        InterleavedFixture, NULL,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_set_up,
        test_account_catchup_flag_changes_follow_holds,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_tear_down);
    g_test_add (
        "/ingestion/delivery-catchup/accounts/fact-mutations-follow-holds",
        InterleavedFixture, NULL,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_set_up,
        test_account_catchup_fact_mutations_follow_holds,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_tear_down);
    g_test_add_func ("/ingestion/delivery-catchup/accounts/"
        "unknown-flag-target-holds",
        test_account_catchup_unknown_flag_target_holds_account);
    g_test_add ("/ingestion/delivery-catchup/accounts/holds-unselectable",
        InterleavedFixture, NULL,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_set_up,
        test_account_catchup_holds_unselectable_account,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_tear_down);
    g_test_add ("/ingestion/delivery-catchup/accounts/held-pass-idempotent",
        InterleavedFixture, NULL,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_set_up,
        test_account_catchup_held_pass_is_idempotent,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_tear_down);
    g_test_add ("/ingestion/delivery-catchup/accounts/held-recovers",
        InterleavedFixture, NULL,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_set_up,
        test_account_catchup_held_account_recovers_like_rebuild,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_tear_down);
    g_test_add ("/ingestion/delivery-catchup/accounts/legacy-fails-on-hold",
        InterleavedFixture, NULL,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_set_up,
        test_account_catchup_without_report_fails_on_hold,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_tear_down);
    g_test_add ("/ingestion/delivery-catchup/accounts/resume-skips-backlog",
        InterleavedFixture, NULL,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_set_up,
        test_account_catchup_resume_skips_scanned_backlog,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_tear_down);
    g_test_add ("/ingestion/delivery-catchup/accounts/"
        "resume-past-checkpoint-keeps-checkpoint",
        InterleavedFixture, NULL,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_set_up,
        test_account_catchup_resume_past_checkpoint_keeps_checkpoint,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_tear_down);
    g_test_add ("/ingestion/delivery-catchup/accounts/"
        "resume-without-new-records-keeps-cursor",
        InterleavedFixture, NULL,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_set_up,
        test_account_catchup_resume_without_new_records_keeps_cursor,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_tear_down);
    g_test_add ("/ingestion/delivery-catchup/accounts/resume-adds-new-hold",
        InterleavedFixture, NULL,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_set_up,
        test_account_catchup_resume_adds_new_hold_after_prior,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_tear_down);
    g_test_add ("/ingestion/delivery-catchup/accounts/stale-cursor-aborts",
        InterleavedFixture, NULL,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_set_up,
        test_account_catchup_stale_cursor_aborts,
        (void (*)(InterleavedFixture *, gconstpointer))
        interleaved_fixture_tear_down);
    g_test_add_func ("/ingestion/delivery-catchup/accounts/"
        "resume-at-checkpoint-advances",
        test_account_catchup_resume_at_checkpoint_advances);
    g_test_add_func ("/ingestion/delivery-catchup/accounts/first-record-held",
        test_account_catchup_hold_on_first_record_keeps_no_checkpoint);
    ADD_OBJECT_TEST ("missing-holds-account",
        test_account_catchup_missing_object_holds_account);
    ADD_OBJECT_TEST ("hash-mismatch-holds-account",
        test_account_catchup_hash_mismatch_holds_account);
    ADD_OBJECT_TEST ("size-mismatch-holds-account",
        test_account_catchup_size_mismatch_holds_account);
    ADD_OBJECT_TEST ("unreadable-holds-account",
        test_account_catchup_unreadable_object_holds_account);
    ADD_OBJECT_TEST ("restored-object-recovers",
        test_account_catchup_restored_object_recovers);
    ADD_OBJECT_TEST ("resume-holds-new-missing-object",
        test_account_catchup_resume_holds_new_missing_object);
    ADD_OBJECT_TEST ("legacy-fails-on-missing-object",
        test_account_catchup_without_report_fails_on_missing_object);
    ADD_OBJECT_TEST ("missing-root-aborts-pass",
        test_account_catchup_missing_object_root_aborts_pass);
    g_test_add_func ("/ingestion/delivery-catchup/accounts/objects/"
        "shared-missing-holds-both-accounts",
        test_account_catchup_shared_missing_object_holds_both_accounts);
    g_test_add_func ("/ingestion/delivery-catchup/accounts/objects/"
        "missing-mid-run-holds-whole-run",
        test_account_catchup_missing_object_mid_run_holds_whole_run);
    g_test_add_func ("/ingestion/delivery-catchup/missing-object",
        test_missing_object_fails_single_inbox_catchup);

    g_test_add_func ("/ingestion/delivery-catchup/accounts/routes-by-account",
        test_account_catchup_routes_deliveries_by_account);
    g_test_add_func ("/ingestion/delivery-catchup/accounts/"
        "rejects-delivery-without-account",
        test_account_catchup_rejects_delivery_without_account);
    g_test_add_func ("/ingestion/delivery-catchup/accounts/rebuild-keeps-uids",
        test_account_catchup_rebuild_keeps_uids);

    g_test_add_func ("/ingestion/delivery-catchup/no-checkpoint",
        test_no_checkpoint_materializes_two_deliveries);
    g_test_add_func ("/ingestion/delivery-catchup/partial-trailing-record",
        test_partial_trailing_record_fails_without_materializing);
    g_test_add_func ("/ingestion/delivery-catchup/checksum-mismatch",
        test_checksum_mismatch_fails_without_materializing);
    g_test_add_func ("/ingestion/delivery-catchup/first-record-corruption",
        test_first_record_corruption_fails_without_materializing);
    g_test_add_func ("/ingestion/delivery-catchup/existing-checkpoint",
        test_existing_checkpoint_materializes_suffix);
    g_test_add_func ("/ingestion/delivery-catchup/checkpoint-at-eof",
        test_checkpoint_at_eof_leaves_state_unchanged);
    g_test_add_func ("/ingestion/delivery-catchup/bad-checkpoint-seek",
        test_bad_checkpoint_seek_fails_without_materializing);

    return g_test_run ();
}
