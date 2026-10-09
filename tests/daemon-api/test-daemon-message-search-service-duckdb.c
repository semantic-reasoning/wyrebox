#include "wyrebox-daemon-message-search-dispatcher.h"
#include "wyrebox-daemon-message-search-duckdb.h"
#include "wyrebox-delivery-catchup.h"
#include "wyrebox-delivery-materializer.h"
#include "wyrebox-eml-ingestor.h"
#include "wyrebox-journal-reader.h"
#include "wyrebox-journal-writer.h"
#include "wyrebox-local-object-store.h"
#include "wyrebox-schema-metadata-store.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

/* Unix microseconds for 2024-01-02 00:00:00 UTC and 2024-01-03. */
#define JAN_2_UNIX_US G_GINT64_CONSTANT (1704153600000000)
#define JAN_3_UNIX_US G_GINT64_CONSTANT (1704240000000000)

typedef struct
{
    gchar *catalog_path;
    gchar *object_root;
    gchar *journal_root;
    WyreboxDaemonMessageSearchService *service;
} SearchFixture;

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
search_fixture_clear (SearchFixture *fixture)
{
    g_clear_object (&fixture->service);
    if (fixture->object_root != NULL)
        remove_tree (fixture->object_root);
    if (fixture->journal_root != NULL)
        remove_tree (fixture->journal_root);
    if (fixture->catalog_path != NULL) {
        g_autofree gchar *dir = g_path_get_dirname (fixture->catalog_path);

        remove_tree (dir);
    }

    g_clear_pointer (&fixture->catalog_path, g_free);
    g_clear_pointer (&fixture->object_root, g_free);
    g_clear_pointer (&fixture->journal_root, g_free);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (SearchFixture, search_fixture_clear)
/* *INDENT-ON* */

static gchar *
create_bootstrap_catalog (void)
{
    g_autofree gchar *dir = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

    dir = g_dir_make_tmp ("wyrebox-daemon-search-catalog-XXXXXX", &error);
    g_assert_no_error (error);

    path = g_build_filename (dir, "catalog.duckdb", NULL);
    store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
    g_assert_no_error (error);
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
ingest_message (WyreboxEmlIngestor *ingestor, const char *from,
    const char *subject, const char *date)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GBytes) bytes = NULL;
    g_auto (WyreboxEmlIngestResult) result = { 0 };
    g_autofree char *date_header = date != NULL ?
        g_strdup_printf ("Date: %s\r\n", date) : g_strdup ("");
    g_autofree char *message = g_strdup_printf ("From: %s\r\n"
            "To: recipient@example.net\r\n"
            "Subject: %s\r\n" "%s" "\r\n" "body\r\n", from, subject,
            date_header);
    gsize length = strlen (message);

    bytes = g_bytes_new_take (g_steal_pointer (&message), length);
    g_assert_true (wyrebox_eml_ingestor_ingest_bytes (ingestor, bytes,
        &result, &error));
    g_assert_no_error (error);
}

static void
run_catchup (const SearchFixture *fixture)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxSchemaMetadataStore) metadata_store = NULL;
    g_autoptr (WyreboxJournalReader) reader = NULL;
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxDeliveryMaterializer) materializer = NULL;

    metadata_store = wyrebox_schema_metadata_store_new_duckdb
            (fixture->catalog_path, &error);
    g_assert_no_error (error);
    reader = wyrebox_journal_reader_new (fixture->journal_root, &error);
    g_assert_no_error (error);
    object_store = wyrebox_local_object_store_new (fixture->object_root,
            &error);
    g_assert_no_error (error);
    materializer = wyrebox_delivery_materializer_new_duckdb
            (fixture->catalog_path, &error);
    g_assert_no_error (error);

    g_assert_true (wyrebox_delivery_catchup_materialize_inbox (metadata_store,
        reader, object_store, materializer, "account-1", &error));
    g_assert_no_error (error);
}

static void
exec_catalog_sql (const gchar *catalog_path, const gchar *sql)
{
    duckdb_database database = NULL;
    duckdb_connection connection = NULL;
    duckdb_result result;

    g_assert_cmpint (duckdb_open (catalog_path, &database), ==, DuckDBSuccess);
    g_assert_cmpint (duckdb_connect (database, &connection), ==,
        DuckDBSuccess);
    if (duckdb_query (connection, sql, &result) != DuckDBSuccess)
        g_error ("duckdb query failed: %s", duckdb_result_error (&result));
    duckdb_destroy_result (&result);
    duckdb_disconnect (&connection);
    duckdb_close (&database);
}

/*
 * account-1's INBOX holds UIDs 1-4; the derived view "view-important"
 * (UIDVALIDITY 21) holds INBOX UIDs 1 and 3 as view UIDs 1 and 2.
 */
static SearchFixture
create_search_fixture (void)
{
    g_auto (SearchFixture) fixture = { 0 };
    SearchFixture out = { 0 };
    g_autoptr (GError) error = NULL;
    g_autoptr (WyreboxLocalObjectStore) object_store = NULL;
    g_autoptr (WyreboxJournalWriter) writer = NULL;
    g_autoptr (WyreboxEmlIngestor) ingestor = NULL;

    fixture.catalog_path = create_bootstrap_catalog ();
    fixture.object_root =
        g_dir_make_tmp ("wyrebox-daemon-search-objects-XXXXXX", NULL);
    fixture.journal_root =
        g_dir_make_tmp ("wyrebox-daemon-search-journal-XXXXXX", NULL);

    object_store = wyrebox_local_object_store_new (fixture.object_root,
            &error);
    g_assert_no_error (error);
    writer = wyrebox_journal_writer_new (fixture.journal_root, &error);
    g_assert_no_error (error);
    ingestor = wyrebox_eml_ingestor_new_with_journal (object_store, writer);

    ingest_message (ingestor, "Alice <alice@Example.COM>",
        "Quarterly Report", "Mon, 1 Jan 2024 12:00:00 +0000");
    ingest_message (ingestor, "bob@other.org", "Größe der Datei 100%",
        "Tue, 2 Jan 2024 12:00:00 +0000");
    ingest_message (ingestor, "carol@example.com", "quarterly plan",
        "Wed, 3 Jan 2024 12:00:00 +0000");
    ingest_message (ingestor, "dave@example.com", "Ünïcode notes", NULL);
    run_catchup (&fixture);

    exec_catalog_sql (fixture.catalog_path,
        "INSERT INTO derived_views (view_id, account_id, imap_name, "
        "definition_ref, is_selectable, is_visible) VALUES "
        "('view-important', 'account-1', 'Important', 'rule:important', "
        "TRUE, TRUE);");
    exec_catalog_sql (fixture.catalog_path,
        "INSERT INTO mailbox_uid_state (account_id, namespace_kind, "
        "namespace_id, uidnext, uidvalidity) VALUES "
        "('account-1', 'derived_view', 'view-important', 3, 21);");
    exec_catalog_sql (fixture.catalog_path,
        "INSERT INTO derived_view_memberships (membership_id, account_id, "
        "view_id, message_id, uid, is_visible, rule_version_hash, "
        "materialized_at_unix_us) "
        "SELECT 'dvm-' || uid, account_id, 'view-important', message_id, "
        "CASE uid WHEN 1 THEN 1 ELSE 2 END, TRUE, 'hash', 1 "
        "FROM mailbox_memberships WHERE account_id = 'account-1' "
        "AND mailbox_id = 'mailbox-inbox' AND uid IN (1, 3);");

    fixture.service = wyrebox_daemon_message_search_service_new_duckdb
            (fixture.catalog_path, &error);
    g_assert_no_error (error);
    g_assert_nonnull (fixture.service);

    out = fixture;
    memset (&fixture, 0, sizeof (fixture));
    return out;
}

static void
dispatch_search (const SearchFixture *fixture, const char *account_identity,
    const char *mailbox_id, WyreboxDaemonMailboxListEntryKind namespace_kind,
    guint64 uid_validity, const WyreboxDaemonMessageSearchCriterion *criteria,
    guint n_criteria, WyreboxDaemonResponseFrame *out_frame)
{
    g_autoptr (GError) error = NULL;
    g_auto (WyreboxDaemonMessageSearchRequest) request = { 0 };

    g_assert_true (wyrebox_daemon_message_search_request_init (&request,
        account_identity, mailbox_id, namespace_kind, uid_validity, criteria,
        n_criteria, &error));
    g_assert_no_error (error);
    g_assert_true (wyrebox_daemon_message_search_dispatch (fixture->service,
        "request-search", "dovecot", account_identity, "dovecot-storage",
        "imap-search-1", &request, out_frame, &error));
    g_assert_no_error (error);
    g_assert_cmpstr (out_frame->request_id, ==, "request-search");
}

static void
assert_search_uids (const SearchFixture *fixture,
    WyreboxDaemonMailboxListEntryKind namespace_kind,
    const WyreboxDaemonMessageSearchCriterion *criteria, guint n_criteria,
    const char *expected_uids)
{
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
    gboolean is_virtual =
        namespace_kind == WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL;
    gsize size = 0;
    const char *data = NULL;

    dispatch_search (fixture, "account-1",
        is_virtual ? "view-important" : "mailbox-inbox", namespace_kind,
        is_virtual ? 21 : 1, criteria, n_criteria, &frame);
    g_assert_cmpint (frame.kind, ==,
        WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
    g_assert_cmpstr (frame.stream_chunk.query_id, ==,
        WYREBOX_DAEMON_MESSAGE_SEARCH_DUCKDB_QUERY_ID);
    g_assert_null (frame.stream_chunk.message_id);
    g_assert_true (frame.stream_chunk.end_of_stream);
    data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
    g_assert_cmpmem (data, size, expected_uids, strlen (expected_uids));
}

static void
assert_inbox_uids (const SearchFixture *fixture,
    const WyreboxDaemonMessageSearchCriterion *criteria, guint n_criteria,
    const char *expected_uids)
{
    assert_search_uids (fixture, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY,
        criteria, n_criteria, expected_uids);
}

static void
assert_search_fails (const SearchFixture *fixture,
    const char *account_identity, const char *mailbox_id,
    guint64 uid_validity, WyreboxDaemonErrorClass error_class)
{
    g_auto (WyreboxDaemonResponseFrame) frame = { 0 };

    dispatch_search (fixture, account_identity, mailbox_id,
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY, uid_validity, NULL, 0,
        &frame);
    g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
    g_assert_cmpint (frame.error.error_class, ==, error_class);
}

#define TEXT_CRITERION(kind, text) \
        { WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_ ## kind, (char *)(text), 0 }
#define DATE_CRITERION(kind, unix_us) \
        { WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_ ## kind, NULL, (unix_us) }

static void
test_search_without_criteria_returns_all_visible_uids (void)
{
    g_auto (SearchFixture) fixture = create_search_fixture ();

    assert_inbox_uids (&fixture, NULL, 0, "1\n2\n3\n4\n");
}

static void
test_search_matches_subject_ignoring_ascii_case (void)
{
    g_auto (SearchFixture) fixture = create_search_fixture ();
    const WyreboxDaemonMessageSearchCriterion criteria[] = {
        TEXT_CRITERION (SUBJECT_CONTAINS, "QUARTERLY"),
    };

    assert_inbox_uids (&fixture, criteria, 1, "1\n3\n");
}

static void
test_search_compares_non_ascii_subject_letters_literally (void)
{
    g_auto (SearchFixture) fixture = create_search_fixture ();
    const WyreboxDaemonMessageSearchCriterion folded_ascii[] = {
        TEXT_CRITERION (SUBJECT_CONTAINS, "größe DER"),
    };
    const WyreboxDaemonMessageSearchCriterion upper_umlaut[] = {
        TEXT_CRITERION (SUBJECT_CONTAINS, "GRÖßE"),
    };
    const WyreboxDaemonMessageSearchCriterion lower_umlaut[] = {
        TEXT_CRITERION (SUBJECT_CONTAINS, "ünïcode"),
    };

    assert_inbox_uids (&fixture, folded_ascii, 1, "2\n");
    assert_inbox_uids (&fixture, upper_umlaut, 1, "");
    assert_inbox_uids (&fixture, lower_umlaut, 1, "");
}

static void
test_search_treats_wildcards_literally (void)
{
    g_auto (SearchFixture) fixture = create_search_fixture ();
    const WyreboxDaemonMessageSearchCriterion percent[] = {
        TEXT_CRITERION (SUBJECT_CONTAINS, "100%"),
    };
    const WyreboxDaemonMessageSearchCriterion underscore[] = {
        TEXT_CRITERION (SUBJECT_CONTAINS, "quarterly_"),
    };

    assert_inbox_uids (&fixture, percent, 1, "2\n");
    assert_inbox_uids (&fixture, underscore, 1, "");
}

static void
test_search_matches_from_and_sender_domain (void)
{
    g_auto (SearchFixture) fixture = create_search_fixture ();
    const WyreboxDaemonMessageSearchCriterion from[] = {
        TEXT_CRITERION (FROM_CONTAINS, "ALICE"),
    };
    const WyreboxDaemonMessageSearchCriterion domain[] = {
        TEXT_CRITERION (SENDER_DOMAIN, "EXAMPLE.com"),
    };
    const WyreboxDaemonMessageSearchCriterion partial_domain[] = {
        TEXT_CRITERION (SENDER_DOMAIN, "example"),
    };

    assert_inbox_uids (&fixture, from, 1, "1\n");
    assert_inbox_uids (&fixture, domain, 1, "1\n3\n4\n");
    assert_inbox_uids (&fixture, partial_domain, 1, "");
}

static void
test_search_matches_date_range (void)
{
    g_auto (SearchFixture) fixture = create_search_fixture ();
    const WyreboxDaemonMessageSearchCriterion range[] = {
        DATE_CRITERION (SENT_SINCE, JAN_2_UNIX_US),
        DATE_CRITERION (SENT_BEFORE, JAN_3_UNIX_US),
    };
    const WyreboxDaemonMessageSearchCriterion since[] = {
        DATE_CRITERION (SENT_SINCE, G_MININT64),
    };

    assert_inbox_uids (&fixture, range, G_N_ELEMENTS (range), "2\n");
    assert_inbox_uids (&fixture, since, 1, "1\n2\n3\n");
}

static void
test_search_combines_criteria_with_and (void)
{
    g_auto (SearchFixture) fixture = create_search_fixture ();
    const WyreboxDaemonMessageSearchCriterion criteria[] = {
        TEXT_CRITERION (SUBJECT_CONTAINS, "quarterly"),
        TEXT_CRITERION (SENDER_DOMAIN, "example.com"),
        DATE_CRITERION (SENT_SINCE, JAN_2_UNIX_US),
    };
    const WyreboxDaemonMessageSearchCriterion no_match[] = {
        TEXT_CRITERION (SUBJECT_CONTAINS, "quarterly"),
        TEXT_CRITERION (FROM_CONTAINS, "bob"),
    };

    assert_inbox_uids (&fixture, criteria, G_N_ELEMENTS (criteria), "3\n");
    assert_inbox_uids (&fixture, no_match, G_N_ELEMENTS (no_match), "");
}

static void
test_search_returns_derived_view_uids (void)
{
    g_auto (SearchFixture) fixture = create_search_fixture ();
    const WyreboxDaemonMessageSearchCriterion criteria[] = {
        TEXT_CRITERION (FROM_CONTAINS, "carol"),
    };

    assert_search_uids (&fixture, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
        NULL, 0, "1\n2\n");
    assert_search_uids (&fixture, WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL,
        criteria, 1, "2\n");
}

static void
test_search_excludes_hidden_memberships (void)
{
    g_auto (SearchFixture) fixture = create_search_fixture ();

    g_clear_object (&fixture.service);
    exec_catalog_sql (fixture.catalog_path,
        "UPDATE mailbox_memberships SET is_visible = FALSE WHERE uid = 2;");
    fixture.service = wyrebox_daemon_message_search_service_new_duckdb
            (fixture.catalog_path, NULL);
    g_assert_nonnull (fixture.service);

    assert_inbox_uids (&fixture, NULL, 0, "1\n3\n4\n");
}

static void
test_search_reports_mailbox_errors (void)
{
    g_auto (SearchFixture) fixture = create_search_fixture ();

    assert_search_fails (&fixture, "account-1", "mailbox-missing", 1,
        WYREBOX_DAEMON_ERROR_NOT_FOUND);
    assert_search_fails (&fixture, "account-1", "mailbox-inbox", 2,
        WYREBOX_DAEMON_ERROR_CONFLICT);
    assert_search_fails (&fixture, "account-2", "mailbox-inbox", 1,
        WYREBOX_DAEMON_ERROR_NOT_FOUND);

    g_clear_object (&fixture.service);
    exec_catalog_sql (fixture.catalog_path,
        "UPDATE mailboxes SET is_selectable = FALSE "
        "WHERE mailbox_id = 'mailbox-inbox';");
    fixture.service = wyrebox_daemon_message_search_service_new_duckdb
            (fixture.catalog_path, NULL);
    g_assert_nonnull (fixture.service);
    assert_search_fails (&fixture, "account-1", "mailbox-inbox", 1,
        WYREBOX_DAEMON_ERROR_NOT_FOUND);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/daemon/message-search-service-duckdb/no-criteria",
        test_search_without_criteria_returns_all_visible_uids);
    g_test_add_func ("/daemon/message-search-service-duckdb/subject-ascii-case",
        test_search_matches_subject_ignoring_ascii_case);
    g_test_add_func ("/daemon/message-search-service-duckdb/"
        "subject-non-ascii-literal",
        test_search_compares_non_ascii_subject_letters_literally);
    g_test_add_func ("/daemon/message-search-service-duckdb/"
        "wildcards-literal", test_search_treats_wildcards_literally);
    g_test_add_func ("/daemon/message-search-service-duckdb/"
        "from-and-sender-domain", test_search_matches_from_and_sender_domain);
    g_test_add_func ("/daemon/message-search-service-duckdb/date-range",
        test_search_matches_date_range);
    g_test_add_func ("/daemon/message-search-service-duckdb/and-combination",
        test_search_combines_criteria_with_and);
    g_test_add_func ("/daemon/message-search-service-duckdb/derived-view",
        test_search_returns_derived_view_uids);
    g_test_add_func ("/daemon/message-search-service-duckdb/hidden-members",
        test_search_excludes_hidden_memberships);
    g_test_add_func ("/daemon/message-search-service-duckdb/mailbox-errors",
        test_search_reports_mailbox_errors);

    return g_test_run ();
}
