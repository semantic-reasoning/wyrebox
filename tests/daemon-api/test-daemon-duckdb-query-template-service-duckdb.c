#include "wyrebox-daemon-duckdb-query-template-dispatcher.h"
#include "wyrebox-schema-metadata-store.h"

#include <duckdb.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

static void
duckdb_connection_clear (duckdb_connection *connection)
{
  duckdb_disconnect (connection);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_connection, duckdb_connection_clear)
/* *INDENT-ON* */

static void
duckdb_database_clear (duckdb_database *database)
{
  duckdb_close (database);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_database, duckdb_database_clear)
/* *INDENT-ON* */

static void
exec_sql (duckdb_connection connection, const gchar *sql)
{
  duckdb_result result = { 0 };
  duckdb_state state = duckdb_query (connection, sql, &result);

  if (state != DuckDBSuccess)
    g_error ("duckdb query failed: %s", duckdb_result_error (&result));

  duckdb_destroy_result (&result);
}

static gchar *
create_temp_catalog_path (void)
{
  gint fd = -1;
  gchar *path = NULL;

  fd = g_file_open_tmp ("wyrebox-duckdb-query-template-XXXXXX.duckdb", &path,
      NULL);
  g_assert_cmpint (fd, >=, 0);
  g_assert_nonnull (path);

  g_assert_true (g_close (fd, NULL));
  (void) g_remove (path);
  return path;
}

static void
bootstrap_catalog (const gchar *path)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxSchemaMetadataStore) store = NULL;

  store = wyrebox_schema_metadata_store_new_duckdb (path, &error);
  g_assert_no_error (error);
  g_assert_nonnull (store);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_LEGACY_BOOTSTRAP,
          0, 1, &error));
  g_assert_no_error (error);

  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_TABLE,
          2, 3, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_SENDER_DOMAIN,
          5, 6, &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_DATE_UNIX_US,
          6, wyrebox_schema_migration_get_current_schema_version (), &error));
  g_assert_no_error (error);
  g_assert_true (wyrebox_schema_metadata_store_apply_migration_operation (store,
          WYREBOX_SCHEMA_METADATA_STORE_MIGRATION_OPERATION_ADD_MESSAGE_HEADER_PROVENANCE_SPANS,
          8, wyrebox_schema_migration_get_current_schema_version (), &error));
  g_assert_no_error (error);
}

static void
seed_catalog (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO accounts (account_id) VALUES "
      "('account-1'), ('account-2');");
  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('message-b', 'account-1', 'object-b', 1, 1),"
      "('message-a', 'account-1', 'object-a', 2, 2),"
      "('message-hidden', 'account-1', 'object-hidden', 3, 3),"
      "('message-other-mailbox', 'account-1', 'object-other-mailbox', 4, 4),"
      "('message-other-account', 'account-2', 'object-other-account', 5, 5);");
  exec_sql (connection,
      "INSERT INTO mailbox_uid_state (account_id, namespace_kind, "
      "namespace_id, uidnext, uidvalidity) VALUES "
      "('account-1', 'mailbox', 'mailbox-inbox', 4, 77),"
      "('account-1', 'mailbox', 'mailbox-archive', 2, 88),"
      "('account-2', 'mailbox', 'mailbox-inbox', 3, 99),"
      "('account-1', 'derived_view', 'view-important', 4, 177),"
      "('account-1', 'derived_view', 'view-archive', 2, 188),"
      "('account-2', 'derived_view', 'view-important', 6, 199),"
      "('account-1', 'derived_view', 'mailbox-inbox', 2, 55);");
  exec_sql (connection,
      "INSERT INTO mailbox_memberships (membership_id, account_id, "
      "mailbox_id, message_id, uid, internal_date_unix_us, journal_offset, "
      "journal_sequence, is_visible) VALUES "
      "('membership-b', 'account-1', 'mailbox-inbox', 'message-b', 2, 1, 1, "
      "1, TRUE),"
      "('membership-a', 'account-1', 'mailbox-inbox', 'message-a', 1, 2, 2, "
      "2, TRUE),"
      "('membership-hidden', 'account-1', 'mailbox-inbox', "
      "'message-hidden', 3, 3, 3, 3, FALSE),"
      "('membership-other-mailbox', 'account-1', 'mailbox-archive', "
      "'message-other-mailbox', 4, 4, 4, 4, TRUE),"
      "('membership-other-account', 'account-2', 'mailbox-inbox', "
      "'message-other-account', 5, 5, 5, 5, TRUE);");
  exec_sql (connection,
      "INSERT INTO derived_view_memberships (membership_id, account_id, "
      "view_id, message_id, uid, is_visible, rule_version_hash, "
      "materialized_at_unix_us) VALUES "
      "('derived-membership-b', 'account-1', 'view-important', "
      "'message-b', 2, TRUE, 'rule-hash-1', 10),"
      "('derived-membership-a', 'account-1', 'view-important', "
      "'message-a', 1, TRUE, 'rule-hash-1', 11),"
      "('derived-membership-hidden', 'account-1', 'view-important', "
      "'message-hidden', 3, FALSE, 'rule-hash-1', 12),"
      "('derived-membership-other-view', 'account-1', 'view-archive', "
      "'message-other-mailbox', 4, TRUE, 'rule-hash-archive', 13),"
      "('derived-membership-other-account', 'account-2', 'view-important', "
      "'message-other-account', 5, TRUE, 'rule-hash-other-account', 14);");
}

static void
seed_sql_looking_mailbox (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('message-sql-looking', 'account-1', 'object-sql-looking', 6, 6);");
  exec_sql (connection,
      "INSERT INTO mailbox_uid_state (account_id, namespace_kind, "
      "namespace_id, uidnext, uidvalidity) VALUES "
      "('account-1', 'mailbox', 'mailbox''; DROP TABLE messages; --', 2, "
      "123);");
  exec_sql (connection,
      "INSERT INTO mailbox_memberships (membership_id, account_id, "
      "mailbox_id, message_id, uid, internal_date_unix_us, journal_offset, "
      "journal_sequence, is_visible) VALUES "
      "('membership-sql-looking', 'account-1', "
      "'mailbox''; DROP TABLE messages; --', 'message-sql-looking', 6, 6, "
      "6, 6, TRUE);");
}

static void
seed_sql_looking_derived_view (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('message-sql-looking-view', 'account-1', 'object-sql-looking-view', "
      "9, 9);");
  exec_sql (connection,
      "INSERT INTO mailbox_uid_state (account_id, namespace_kind, "
      "namespace_id, uidnext, uidvalidity) VALUES "
      "('account-1', 'derived_view', 'view''; DROP TABLE messages; --', 2, "
      "223);");
  exec_sql (connection,
      "INSERT INTO derived_view_memberships (membership_id, account_id, "
      "view_id, message_id, uid, is_visible, rule_version_hash, "
      "materialized_at_unix_us) VALUES "
      "('derived-membership-sql-looking', 'account-1', "
      "'view''; DROP TABLE messages; --', 'message-sql-looking-view', 6, "
      "TRUE, 'rule-hash-sql-looking', 16);");
}

static void
seed_message_header (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, to_addr, cc_addr, bcc_addr, date_raw, journal_offset, "
      "journal_sequence, message_id_span_start, message_id_span_end, "
      "subject_span_start, subject_span_end) VALUES "
      "('message-a', '<message-a@example.test>', 0, 'Subject A', "
      "'Alice <alice@example.test>', 'Bob <bob@example.test>', "
      "'Carol <carol@example.test>', 'Blind <blind@example.test>', "
      "'Mon, 01 Jan 2024 00:00:00 +0000', 20, 21, 101, 109, 10, 18);");
}

static void
seed_message_facts (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO message_facts (fact_id, account_id, message_id, "
      "object_id, predicate, args_json, source, confidence_ppm, "
      "created_at_unix_us, retracted_at_unix_us, journal_offset, "
      "journal_sequence) VALUES "
      "('fact-b', 'account-1', 'message-a', 'object-a', 'mention', "
      "'[\"Project A\"]', 'llm', 910000, 1000, 0, 22, 23),"
      "('fact-a', 'account-1', 'message-a', 'object-a', 'sender', "
      "'[\"Alice\", \"Alice <alice@example.test>\"]', 'rule', 990000, "
      "900, 0, 21, 22),"
      "('fact-other-account', 'account-2', 'message-other-account', "
      "'object-other-account', 'mention', '[\"Project B\"]', 'rule', "
      "870000, 2000, 0, 24, 25);");
}

static void
seed_message_facts_with_provenance (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO message_facts (fact_id, account_id, message_id, "
      "object_id, predicate, args_json, source, confidence_ppm, "
      "created_at_unix_us, retracted_at_unix_us, journal_offset, "
      "journal_sequence) VALUES "
      "('fact-provenance', 'account-1', 'message-a', 'object-a', 'mention', "
      "'[\"Subject A\"]', 'header:subject', 980000, 950, 0, 23, 24);");
}

static void
seed_messages_by_from_addr_headers (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, to_addr, cc_addr, bcc_addr, date_raw, journal_offset, "
      "journal_sequence) VALUES "
      "('message-b', '<message-b@example.test>', 0, 'Subject B', "
      "'Alice <alice@example.test>', 'Bob <bob@example.test>', NULL, NULL, "
      "'Sun, 31 Dec 2023 23:00:00 +0000', 18, 19),"
      "('message-other-account', '<message-other-account@example.test>', 0, "
      "'Subject Other Account', 'Alice <alice@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, "
      "'Wed, 03 Jan 2024 00:00:00 +0000', 40, 41);");
}

static void
seed_messages_by_subject_headers (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, to_addr, cc_addr, bcc_addr, date_raw, journal_offset, "
      "journal_sequence) VALUES "
      "('message-b', '<message-b@example.test>', 0, 'Subject B', "
      "'Alice <alice@example.test>', 'Bob <bob@example.test>', NULL, NULL, "
      "'Sun, 31 Dec 2023 23:00:00 +0000', 18, 19),"
      "('message-hidden', '<message-hidden@example.test>', 0, 'Subject B', "
      "'Zoe <zoe@example.test>', 'Bob <bob@example.test>', NULL, NULL, "
      "'Thu, 04 Jan 2024 00:00:00 +0000', 42, 43);");
}

static void
seed_high_cardinality_subject_headers (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  for (guint i = 0; i < 105; i++) {
    g_autoptr (GString) sql = NULL;

    sql = g_string_new (NULL);
    g_string_append_printf (sql,
        "INSERT INTO messages (message_id, account_id, object_id, "
        "journal_offset, journal_sequence) VALUES "
        "('message-volume-%03u', 'account-1', 'object-volume-%03u', "
        "%" G_GUINT64_FORMAT ", %" G_GUINT64_FORMAT ");",
        i, i, (guint64) 1000 + i, (guint64) 1000 + i);
    exec_sql (connection, sql->str);

    g_string_truncate (sql, 0);
    g_string_append_printf (sql,
        "INSERT INTO message_headers ("
        "message_id, rfc_message_id, duplicate_message_id_count, subject, "
        "from_addr, to_addr, cc_addr, bcc_addr, date_raw, journal_offset, "
        "journal_sequence) VALUES "
        "('message-volume-%03u', '<message-volume-%03u@example.test>', 0, "
        "'High Volume Subject', 'Sender %03u <sender%03u@example.test>', "
        "'Bob <bob@example.test>', NULL, NULL, "
        "'Mon, 01 Jan 2024 00:00:00 +0000', "
        "%" G_GUINT64_FORMAT ", %" G_GUINT64_FORMAT ");",
        i, i, i, i, (guint64) 2000 + i, (guint64) 2000 + i);
    exec_sql (connection, sql->str);
  }
}

static void
seed_subject_contains_headers (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('message-contains-a', 'account-1', 'object-contains-a', 30, 30),"
      "('message-contains-b', 'account-1', 'object-contains-b', 31, 31),"
      "('message-contains-wildcard', 'account-1', "
      "'object-contains-wildcard', 32, 32),"
      "('message-contains-underscore', 'account-1', "
      "'object-contains-underscore', 33, 33),"
      "('message-contains-backslash', 'account-1', "
      "'object-contains-backslash', 34, 34),"
      "('message-contains-accented-latin', 'account-1', "
      "'object-contains-accented-latin', 35, 35),"
      "('message-contains-greek', 'account-1', "
      "'object-contains-greek', 36, 36),"
      "('message-contains-turkish-dotted-i', 'account-1', "
      "'object-contains-turkish-dotted-i', 37, 37),"
      "('message-contains-turkish-dotless-i', 'account-1', "
      "'object-contains-turkish-dotless-i', 38, 38),"
      "('message-contains-sharp-s', 'account-1', "
      "'object-contains-sharp-s', 39, 39),"
      "('message-contains-decoded-rfc2047', 'account-1', "
      "'object-contains-decoded-rfc2047', 40, 40),"
      "('message-contains-null', 'account-1', 'object-contains-null', "
      "41, 41),"
      "('message-contains-other-account', 'account-2', "
      "'object-contains-other-account', 42, 42);");
  exec_sql (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, to_addr, cc_addr, bcc_addr, date_raw, journal_offset, "
      "journal_sequence) VALUES "
      "('message-contains-a', '<message-contains-a@example.test>', 0, "
      "'Quarterly Subject Review', 'Sender <sender@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, NULL, 80, 81),"
      "('message-contains-b', '<message-contains-b@example.test>', 0, "
      "'embedded subject token', 'Sender <sender@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, NULL, 82, 83),"
      "('message-contains-wildcard', "
      "'<message-contains-wildcard@example.test>', 0, "
      "'Budget 100% literal', 'Sender <sender@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, NULL, 84, 85),"
      "('message-contains-underscore', "
      "'<message-contains-underscore@example.test>', 0, "
      "'Release tag build_42', 'Sender <sender@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, NULL, 86, 87),"
      "('message-contains-backslash', "
      "'<message-contains-backslash@example.test>', 0, "
      "'Path C:\\\\Temp', 'Sender <sender@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, NULL, 88, 89),"
      "('message-contains-accented-latin', "
      "'<message-contains-accented-latin@example.test>', 0, "
      "'résumé de projet', 'Sender <sender@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, NULL, 90, 91),"
      "('message-contains-greek', "
      "'<message-contains-greek@example.test>', 0, "
      "'συνάντηση έργου', 'Sender <sender@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, NULL, 92, 93),"
      "('message-contains-turkish-dotted-i', "
      "'<message-contains-turkish-dotted-i@example.test>', 0, "
      "'istanbul planı', 'Sender <sender@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, NULL, 94, 95),"
      "('message-contains-turkish-dotless-i', "
      "'<message-contains-turkish-dotless-i@example.test>', 0, "
      "'ışık notu', 'Sender <sender@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, NULL, 96, 97),"
      "('message-contains-sharp-s', "
      "'<message-contains-sharp-s@example.test>', 0, "
      "'straße update', 'Sender <sender@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, NULL, 98, 99),"
      "('message-contains-decoded-rfc2047', "
      "'<message-contains-decoded-rfc2047@example.test>', 0, "
      "'café réunion', 'Sender <sender@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, NULL, 100, 101),"
      "('message-contains-null', '<message-contains-null@example.test>', 0, "
      "NULL, 'Sender <sender@example.test>', 'Bob <bob@example.test>', "
      "NULL, NULL, NULL, 102, 103),"
      "('message-contains-other-account', "
      "'<message-contains-other-account@example.test>', 0, "
      "'Quarterly Subject Review', 'Sender <sender@example.test>', "
      "'Bob <bob@example.test>', NULL, NULL, NULL, 104, 105);");
}

static void
seed_messages_by_date_range_headers (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('message-aa', 'account-1', 'object-aa', 20, 2),"
      "('message-null-date', 'account-1', 'object-null-date', 21, 6);");
  exec_sql (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, sender_domain, to_addr, cc_addr, bcc_addr, date_raw, "
      "date_unix_us, journal_offset, journal_sequence) VALUES "
      "('message-b', '<message-b@example.test>', 0, 'Subject B', "
      "'Alice <alice@example.test>', 'example.test', "
      "'Bob <bob@example.test>', NULL, NULL, "
      "'Sun, 31 Dec 2023 23:00:00 +0000', 1704063600000000, 18, 19),"
      "('message-a', '<message-a@example.test>', 0, 'Subject A', "
      "'Alice <alice@example.test>', 'example.test', "
      "'Bob <bob@example.test>', "
      "'Carol <carol@example.test>', 'Blind <blind@example.test>', "
      "'Mon, 01 Jan 2024 00:00:00 +0000', 1704067200000000, 20, 21),"
      "('message-aa', '<message-aa@example.test>', 0, 'Subject AA', "
      "'Ann <ann@example.test>', 'example.test', "
      "'Bob <bob@example.test>', NULL, NULL, "
      "'Tue, 02 Jan 2024 00:00:00 +0000', 1704153600000000, 22, 23),"
      "('message-hidden', '<message-hidden@example.test>', 0, 'Subject C', "
      "'Zoe <zoe@example.test>', 'example.test', "
      "'Bob <bob@example.test>', NULL, NULL, "
      "'Wed, 03 Jan 2024 00:00:00 +0000', 1704240000000000, 42, 43),"
      "('message-null-date', '<message-null-date@example.test>', 0, "
      "'Subject Null Date', 'Null <null@example.test>', 'example.test', "
      "'Bob <bob@example.test>', NULL, NULL, "
      "'Fri, 12 Jun 2026 10:00:00 EST', NULL, 50, 51),"
      "('message-other-account', '<message-other-account@example.test>', 0, "
      "'Subject Other Account', 'Other <other@example.test>', "
      "'example.test', 'Bob <bob@example.test>', NULL, NULL, "
      "'Tue, 02 Jan 2024 00:00:00 +0000', 1704153600000000, 40, 41);");
}

static void
seed_messages_by_sender_domain_headers (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, sender_domain, to_addr, cc_addr, bcc_addr, date_raw, "
      "journal_offset, journal_sequence) VALUES "
      "('message-b', '<message-b@example.test>', 0, 'Subject B', "
      "'Beta <beta@Example.TEST>', 'example.test', "
      "'Bob <bob@example.test>', NULL, NULL, "
      "'Sun, 31 Dec 2023 23:00:00 +0000', 18, 19),"
      "('message-a', '<message-a@example.test>', 0, 'Subject A', "
      "'Alice <alice@example.test>', 'example.test', "
      "'Bob <bob@example.test>', "
      "'Carol <carol@example.test>', 'Blind <blind@example.test>', "
      "'Mon, 01 Jan 2024 00:00:00 +0000', 20, 21),"
      "('message-hidden', '<message-hidden@example.test>', 0, 'Subject B', "
      "'Zoe <zoe@other.test>', 'other.test', "
      "'Bob <bob@example.test>', NULL, NULL, "
      "'Thu, 04 Jan 2024 00:00:00 +0000', 42, 43),"
      "('message-other-account', '<message-other-account@example.test>', 0, "
      "'Subject Other Account', 'Other <other@example.test>', "
      "'example.test', 'Bob <bob@example.test>', NULL, NULL, "
      "'Wed, 03 Jan 2024 00:00:00 +0000', 40, 41);");
}

static void
seed_sql_looking_message (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('message''; DROP TABLE messages; --', 'account-1', "
      "'object-sql-looking-message', 7, 7);");
}

static void
seed_sql_looking_sender_domain_message (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('message-sql-looking-domain', 'account-1', "
      "'object-sql-looking-domain', 12, 12);");
  exec_sql (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, sender_domain, to_addr, cc_addr, bcc_addr, date_raw, "
      "journal_offset, journal_sequence) VALUES "
      "('message-sql-looking-domain', NULL, 0, 'SQL Looking Domain', "
      "'Sender <sender@domain''; drop table messages; -->', "
      "'domain''; drop table messages; --', 'Bob <bob@example.test>', "
      "NULL, NULL, NULL, 70, 71);");
}

static void
seed_sql_looking_from_addr_message (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('message-sql-looking-from', 'account-1', "
      "'object-sql-looking-from', 10, 10);");
  exec_sql (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, to_addr, cc_addr, bcc_addr, date_raw, journal_offset, "
      "journal_sequence) VALUES "
      "('message-sql-looking-from', NULL, 0, 'SQL Looking Sender', "
      "'sender''; DROP TABLE messages; --', 'Bob <bob@example.test>', "
      "NULL, NULL, NULL, 50, 51);");
}

static void
seed_sql_looking_subject_message (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('message-sql-looking-subject', 'account-1', "
      "'object-sql-looking-subject', 11, 11);");
  exec_sql (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, to_addr, cc_addr, bcc_addr, date_raw, journal_offset, "
      "journal_sequence) VALUES "
      "('message-sql-looking-subject', NULL, 0, "
      "'subject''; DROP TABLE messages; --', "
      "'Sender <sender@example.test>', 'Bob <bob@example.test>', "
      "NULL, NULL, NULL, 60, 61);");
}

static void
seed_message_with_escaped_nullable_headers (const gchar *path)
{
  g_auto (duckdb_database) database = NULL;
  g_auto (duckdb_connection) connection = NULL;

  g_assert_cmpint (duckdb_open (path, &database), ==, DuckDBSuccess);
  g_assert_cmpint (duckdb_connect (database, &connection), ==, DuckDBSuccess);

  exec_sql (connection,
      "INSERT INTO messages (message_id, account_id, object_id, "
      "journal_offset, journal_sequence) VALUES "
      "('message-escaped', 'account-1', 'object-escaped', 8, 8);");
  exec_sql (connection,
      "INSERT INTO message_headers ("
      "message_id, rfc_message_id, duplicate_message_id_count, subject, "
      "from_addr, to_addr, cc_addr, bcc_addr, date_raw, journal_offset, "
      "journal_sequence) VALUES "
      "('message-escaped', NULL, 0, 'Subject, with comma', "
      "'Quote \" Sender <quote@example.test>', "
      "'Line\nBreak <line@example.test>', NULL, '', "
      "'Tue, 02 Jan 2024 00:00:00 +0000', 30, 31);");
}

static void
init_request_with_template (WyreboxDaemonDuckDBQueryTemplateRequest *request,
    const gchar *template_id, const gchar *account_id, const gchar *parameter)
{
  const gchar *parameters[] = { parameter, NULL };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (request,
          "query-1", template_id, account_id, parameters, &error));
  g_assert_no_error (error);
}

static void
init_uid_map_request (WyreboxDaemonDuckDBQueryTemplateRequest *request,
    const gchar *account_id, const gchar *mailbox_id)
{
  init_request_with_template (request, "mailbox.uid_map.v1", account_id,
      mailbox_id);
}

static void
init_message_by_id_request (WyreboxDaemonDuckDBQueryTemplateRequest *request,
    const gchar *account_id, const gchar *message_id)
{
  init_request_with_template (request, "message.by_id.v1", account_id,
      message_id);
}

static void
    init_message_facts_by_message_id_request
    (WyreboxDaemonDuckDBQueryTemplateRequest * request,
    const gchar * account_id, const gchar * message_id)
{
  init_request_with_template (request, "message.facts_by_message_id.v1",
      account_id, message_id);
}

static void
init_facts_by_source_request (WyreboxDaemonDuckDBQueryTemplateRequest *request,
    const gchar *account_id, const gchar *source)
{
  init_request_with_template (request, "facts.by_source.v1", account_id,
      source);
}

static void
init_facts_by_fact_id_request (WyreboxDaemonDuckDBQueryTemplateRequest *request,
    const gchar *account_id, const gchar *fact_id)
{
  init_request_with_template (request, "facts.by_fact_id.v1", account_id,
      fact_id);
}

static void
    init_facts_by_fact_id_with_provenance_request
    (WyreboxDaemonDuckDBQueryTemplateRequest * request,
    const gchar * account_id, const gchar * fact_id)
{
  init_request_with_template (request, "facts.by_fact_id_with_provenance.v1",
      account_id, fact_id);
}

static void
init_messages_by_from_addr_request (WyreboxDaemonDuckDBQueryTemplateRequest
    *request, const gchar *account_id, const gchar *from_addr)
{
  const gchar *parameters[] = { from_addr, "100", "0", NULL };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (request,
          "query-1", "messages.by_from_addr.v1", account_id, parameters,
          &error));
  g_assert_no_error (error);
}

static void
init_messages_by_sender_domain_request (WyreboxDaemonDuckDBQueryTemplateRequest
    *request, const gchar *account_id, const gchar *sender_domain)
{
  const gchar *parameters[] = { sender_domain, "100", "0", NULL };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (request,
          "query-1", "messages.by_sender_domain.v1", account_id, parameters,
          &error));
  g_assert_no_error (error);
}

static void
init_messages_by_subject_request (WyreboxDaemonDuckDBQueryTemplateRequest
    *request, const gchar *account_id, const gchar *subject)
{
  const gchar *parameters[] = { subject, "100", "0", NULL };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (request,
          "query-1", "messages.by_subject.v1", account_id, parameters, &error));
  g_assert_no_error (error);
}

static void
init_messages_by_subject_page_request (WyreboxDaemonDuckDBQueryTemplateRequest
    *request, const gchar *account_id, const gchar *subject,
    const gchar *limit, const gchar *offset)
{
  const gchar *parameters[] = { subject, limit, offset, NULL };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (request,
          "query-1", "messages.by_subject.v1", account_id, parameters, &error));
  g_assert_no_error (error);
}

static void
init_messages_subject_contains_request (WyreboxDaemonDuckDBQueryTemplateRequest
    *request, const gchar *account_id, const gchar *subject_term,
    const gchar *limit, const gchar *offset)
{
  const gchar *parameters[] = { subject_term, limit, offset, NULL };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (request,
          "query-1", "messages.subject_contains.v1", account_id, parameters,
          &error));
  g_assert_no_error (error);
}

static void
init_messages_by_date_range_request (WyreboxDaemonDuckDBQueryTemplateRequest
    *request, const gchar *account_id, const gchar *start_unix_us,
    const gchar *end_unix_us)
{
  const gchar *parameters[] = { start_unix_us, end_unix_us, "100", "0",
    NULL
  };
  g_autoptr (GError) error = NULL;

  g_assert_true (wyrebox_daemon_duckdb_query_template_request_init (request,
          "query-1", "messages.by_date_range.v1", account_id, parameters,
          &error));
  g_assert_no_error (error);
}

static gchar *
dispatch_messages_by_sender_domain_csv (const gchar *path,
    const gchar *account_id, const gchar *sender_domain)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_messages_by_sender_domain_request (&request, account_id, sender_domain);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static gchar *
dispatch_derived_view_uid_map_csv (const gchar *path, const gchar *account_id,
    const gchar *view_id)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_request_with_template (&request, "derived_view.uid_map.v1", account_id,
      view_id);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static gchar *
dispatch_uid_map_csv (const gchar *path, const gchar *account_id,
    const gchar *mailbox_id)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_uid_map_request (&request, account_id, mailbox_id);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static gchar *
dispatch_message_by_id_csv (const gchar *path, const gchar *account_id,
    const gchar *message_id)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_message_by_id_request (&request, account_id, message_id);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static gchar *
dispatch_message_facts_by_message_id_csv (const gchar *path,
    const gchar *account_id, const gchar *message_id)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_message_facts_by_message_id_request (&request, account_id, message_id);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static gchar *
dispatch_facts_by_source_csv (const gchar *path, const gchar *account_id,
    const gchar *source)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_facts_by_source_request (&request, account_id, source);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static gchar *
dispatch_facts_by_fact_id_csv (const gchar *path, const gchar *account_id,
    const gchar *fact_id)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_facts_by_fact_id_request (&request, account_id, fact_id);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static gchar *
dispatch_facts_by_fact_id_with_provenance_csv (const gchar *path,
    const gchar *account_id, const gchar *fact_id)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_facts_by_fact_id_with_provenance_request (&request, account_id, fact_id);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static gchar *
dispatch_messages_by_from_addr_csv (const gchar *path, const gchar *account_id,
    const gchar *from_addr)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_messages_by_from_addr_request (&request, account_id, from_addr);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static gchar *
dispatch_messages_by_subject_csv (const gchar *path, const gchar *account_id,
    const gchar *subject)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_messages_by_subject_request (&request, account_id, subject);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static gchar *
dispatch_messages_by_subject_page_csv (const gchar *path,
    const gchar *account_id, const gchar *subject, const gchar *limit,
    const gchar *offset)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_messages_by_subject_page_request (&request, account_id, subject, limit,
      offset);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static gchar *
dispatch_messages_subject_contains_csv (const gchar *path,
    const gchar *account_id, const gchar *subject_term, const gchar *limit,
    const gchar *offset)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_messages_subject_contains_request (&request, account_id, subject_term,
      limit, offset);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static gchar *
dispatch_messages_by_date_range_csv (const gchar *path,
    const gchar *account_id, const gchar *start_unix_us,
    const gchar *end_unix_us)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;
  gconstpointer data = NULL;
  gsize size = 0;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_messages_by_date_range_request (&request, account_id, start_unix_us,
      end_unix_us);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);

  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_STREAM_CHUNK);
  g_assert_cmpuint (frame.stream_chunk.chunk_index, ==, 0);
  g_assert_true (frame.stream_chunk.end_of_stream);

  data = g_bytes_get_data (frame.stream_chunk.bytes, &size);
  return g_strndup (data, size);
}

static gsize
count_substring (const gchar *text, const gchar *needle)
{
  gsize count = 0;
  const gchar *cursor = text;

  while ((cursor = g_strstr_len (cursor, -1, needle)) != NULL) {
    count++;
    cursor += strlen (needle);
  }

  return count;
}

static void
assert_messages_by_date_range_rejects_bound (const gchar *path,
    const gchar *account_id, const gchar *start_unix_us,
    const gchar *end_unix_us)
{
  g_auto (WyreboxDaemonDuckDBQueryTemplateRequest) request = { 0 };
  g_auto (WyreboxDaemonResponseFrame) frame = { 0 };
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_no_error (error);
  g_assert_nonnull (service);

  init_messages_by_date_range_request (&request, account_id, start_unix_us,
      end_unix_us);
  g_assert_true (wyrebox_daemon_duckdb_query_template_dispatch (service,
          "request-1", "admin-cli", account_id, "duckdb-tool",
          "correlation-1", &request, &frame, &error));
  g_assert_no_error (error);
  g_assert_cmpint (frame.kind, ==, WYREBOX_DAEMON_RESPONSE_FRAME_ERROR);
  g_assert_cmpint (frame.error.error_class, ==,
      WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE);
}

static void
test_duckdb_service_returns_derived_view_uid_map_csv (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_derived_view_uid_map_csv (path, "account-1", "view-important");
  g_assert_cmpstr (csv, ==,
      "account_id,view_id,uidvalidity,uid,message_id,object_id,"
      "rule_version_hash\n"
      "account-1,view-important,177,1,message-a,object-a,rule-hash-1\n"
      "account-1,view-important,177,2,message-b,object-b,rule-hash-1\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_derived_view_empty_result_is_header_only (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_derived_view_uid_map_csv (path, "account-1", "view-missing");
  g_assert_cmpstr (csv, ==,
      "account_id,view_id,uidvalidity,uid,message_id,object_id,"
      "rule_version_hash\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_derived_view_isolates_cross_account_rows (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_derived_view_uid_map_csv (path, "account-2", "view-important");
  g_assert_cmpstr (csv, ==,
      "account_id,view_id,uidvalidity,uid,message_id,object_id,"
      "rule_version_hash\n"
      "account-2,view-important,199,5,message-other-account,"
      "object-other-account,rule-hash-other-account\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_treats_sql_looking_derived_view_as_value (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_sql_looking_derived_view (path);

  csv = dispatch_derived_view_uid_map_csv (path, "account-1",
      "view'; DROP TABLE messages; --");
  g_assert_cmpstr (csv, ==,
      "account_id,view_id,uidvalidity,uid,message_id,object_id,"
      "rule_version_hash\n"
      "account-1,view'; DROP TABLE messages; --,223,6,"
      "message-sql-looking-view,object-sql-looking-view,"
      "rule-hash-sql-looking\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_returns_uid_map_csv (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_uid_map_csv (path, "account-1", "mailbox-inbox");
  g_assert_cmpstr (csv, ==,
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n"
      "account-1,mailbox-inbox,77,1,message-a,object-a\n"
      "account-1,mailbox-inbox,77,2,message-b,object-b\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_empty_result_is_header_only (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_uid_map_csv (path, "account-1", "mailbox-missing");
  g_assert_cmpstr (csv, ==,
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_isolates_cross_account_rows (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_uid_map_csv (path, "account-2", "mailbox-inbox");
  g_assert_cmpstr (csv, ==,
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n"
      "account-2,mailbox-inbox,99,5,message-other-account,"
      "object-other-account\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_treats_sql_looking_mailbox_as_value (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_sql_looking_mailbox (path);

  csv = dispatch_uid_map_csv (path, "account-1",
      "mailbox'; DROP TABLE messages; --");
  g_assert_cmpstr (csv, ==,
      "account_id,mailbox_id,uidvalidity,uid,message_id,object_id\n"
      "account-1,mailbox'; DROP TABLE messages; --,123,6,"
      "message-sql-looking,object-sql-looking\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_returns_message_by_id_with_headers (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_header (path);

  csv = dispatch_message_by_id_csv (path, "account-1", "message-a");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-a,object-a,2,2,<message-a@example.test>,Subject A,"
      "Alice <alice@example.test>,Bob <bob@example.test>,"
      "Carol <carol@example.test>,Blind <blind@example.test>,"
      "\"Mon, 01 Jan 2024 00:00:00 +0000\",20,21\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_message_by_id_isolates_cross_account_rows (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_message_by_id_csv (path, "account-1", "message-other-account");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_message_by_id_missing_message_is_header_only (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_message_by_id_csv (path, "account-1", "message-missing");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_treats_sql_looking_message_id_as_value (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_sql_looking_message (path);

  csv = dispatch_message_by_id_csv (path, "account-1",
      "message'; DROP TABLE messages; --");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message'; DROP TABLE messages; --,"
      "object-sql-looking-message,7,7,,,,,,,,,\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_message_by_id_escapes_nullable_headers (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_with_escaped_nullable_headers (path);

  csv = dispatch_message_by_id_csv (path, "account-1", "message-escaped");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-escaped,object-escaped,8,8,,"
      "\"Subject, with comma\","
      "\"Quote \"\" Sender <quote@example.test>\","
      "\"Line\nBreak <line@example.test>\",,,"
      "\"Tue, 02 Jan 2024 00:00:00 +0000\",30,31\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_returns_message_facts_by_message_id_csv (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_facts (path);

  csv = dispatch_message_facts_by_message_id_csv (path, "account-1",
      "message-a");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,fact_id,predicate,args_json,source,"
      "confidence_ppm,created_at_unix_us,retracted_at_unix_us,journal_offset,"
      "journal_sequence\n"
      "account-1,message-a,object-a,fact-a,sender,"
      "\"[\"\"Alice\"\", \"\"Alice <alice@example.test>\"\"]\",rule,"
      "990000,900,0,21,22\n"
      "account-1,message-a,object-a,fact-b,mention,"
      "\"[\"\"Project A\"\"]\",llm,910000,1000,0,22,23\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_returns_facts_by_source_csv (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_facts (path);

  csv = dispatch_facts_by_source_csv (path, "account-1", "rule");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,fact_id,predicate,args_json,source,"
      "confidence_ppm,created_at_unix_us,retracted_at_unix_us,journal_offset,"
      "journal_sequence\n"
      "account-1,message-a,object-a,fact-a,sender,"
      "\"[\"\"Alice\"\", \"\"Alice <alice@example.test>\"\"]\",rule,"
      "990000,900,0,21,22\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_facts_by_source_isolates_cross_account_rows (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_facts (path);

  csv = dispatch_facts_by_source_csv (path, "account-1", "llm");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,fact_id,predicate,args_json,source,"
      "confidence_ppm,created_at_unix_us,retracted_at_unix_us,journal_offset,"
      "journal_sequence\n"
      "account-1,message-a,object-a,fact-b,mention,"
      "\"[\"\"Project A\"\"]\",llm,910000,1000,0,22,23\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_facts_by_source_missing_source_is_header_only (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_facts_by_source_csv (path, "account-1", "missing");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,fact_id,predicate,args_json,source,"
      "confidence_ppm,created_at_unix_us,retracted_at_unix_us,journal_offset,"
      "journal_sequence\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_returns_facts_by_fact_id_csv (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_facts (path);

  csv = dispatch_facts_by_fact_id_csv (path, "account-1", "fact-a");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,fact_id,predicate,args_json,source,"
      "confidence_ppm,created_at_unix_us,retracted_at_unix_us,journal_offset,"
      "journal_sequence\n"
      "account-1,message-a,object-a,fact-a,sender,"
      "\"[\"\"Alice\"\", \"\"Alice <alice@example.test>\"\"]\",rule,"
      "990000,900,0,21,22\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_returns_facts_by_fact_id_with_provenance_csv (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_header (path);
  seed_message_facts (path);
  seed_message_facts_with_provenance (path);

  csv = dispatch_facts_by_fact_id_with_provenance_csv (path, "account-1",
      "fact-provenance");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,fact_id,predicate,args_json,source,"
      "source_span_start,source_span_end,confidence_ppm,created_at_unix_us,"
      "retracted_at_unix_us,journal_offset,journal_sequence\n"
      "account-1,message-a,object-a,fact-provenance,mention,"
      "\"[\"\"Subject A\"\"]\",header:subject,10,18,980000,950,0,23,24\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_facts_by_fact_id_isolates_cross_account_rows (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_facts (path);

  csv = dispatch_facts_by_fact_id_csv (path, "account-1", "fact-other-account");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,fact_id,predicate,args_json,source,"
      "confidence_ppm,created_at_unix_us,retracted_at_unix_us,journal_offset,"
      "journal_sequence\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_facts_by_fact_id_missing_fact_is_header_only (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_facts_by_fact_id_csv (path, "account-1", "fact-missing");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,fact_id,predicate,args_json,source,"
      "confidence_ppm,created_at_unix_us,retracted_at_unix_us,journal_offset,"
      "journal_sequence\n");
  (void) g_remove (path);
}

static void
    test_duckdb_service_message_facts_by_message_id_isolates_cross_account_rows
    (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_facts (path);

  csv = dispatch_message_facts_by_message_id_csv (path, "account-1",
      "message-other-account");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,fact_id,predicate,args_json,source,"
      "confidence_ppm,created_at_unix_us,retracted_at_unix_us,journal_offset,"
      "journal_sequence\n");
  (void) g_remove (path);
}

static void
    test_duckdb_service_message_facts_by_message_id_missing_message_is_header_only
    (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);

  csv = dispatch_message_facts_by_message_id_csv (path, "account-1",
      "message-missing");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,fact_id,predicate,args_json,source,"
      "confidence_ppm,created_at_unix_us,retracted_at_unix_us,journal_offset,"
      "journal_sequence\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_returns_messages_by_from_addr_csv (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_header (path);

  csv = dispatch_messages_by_from_addr_csv (path, "account-1",
      "Alice <alice@example.test>");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-a,object-a,2,2,<message-a@example.test>,Subject A,"
      "Alice <alice@example.test>,Bob <bob@example.test>,"
      "Carol <carol@example.test>,Blind <blind@example.test>,"
      "\"Mon, 01 Jan 2024 00:00:00 +0000\",20,21\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_by_from_addr_missing_sender_is_header_only (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_header (path);

  csv = dispatch_messages_by_from_addr_csv (path, "account-1",
      "alice@example.test");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_by_from_addr_isolates_cross_account_rows (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_header (path);
  seed_messages_by_from_addr_headers (path);

  csv = dispatch_messages_by_from_addr_csv (path, "account-2",
      "Alice <alice@example.test>");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-2,message-other-account,object-other-account,5,5,"
      "<message-other-account@example.test>,Subject Other Account,"
      "Alice <alice@example.test>,Bob <bob@example.test>,,,"
      "\"Wed, 03 Jan 2024 00:00:00 +0000\",40,41\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_by_from_addr_orders_multiple_rows (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_header (path);
  seed_messages_by_from_addr_headers (path);

  csv = dispatch_messages_by_from_addr_csv (path, "account-1",
      "Alice <alice@example.test>");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-b,object-b,1,1,<message-b@example.test>,Subject B,"
      "Alice <alice@example.test>,Bob <bob@example.test>,,,"
      "\"Sun, 31 Dec 2023 23:00:00 +0000\",18,19\n"
      "account-1,message-a,object-a,2,2,<message-a@example.test>,Subject A,"
      "Alice <alice@example.test>,Bob <bob@example.test>,"
      "Carol <carol@example.test>,Blind <blind@example.test>,"
      "\"Mon, 01 Jan 2024 00:00:00 +0000\",20,21\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_treats_sql_looking_from_addr_as_value (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_sql_looking_from_addr_message (path);

  csv = dispatch_messages_by_from_addr_csv (path, "account-1",
      "sender'; DROP TABLE messages; --");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-sql-looking-from,object-sql-looking-from,10,10,,"
      "SQL Looking Sender,sender'; DROP TABLE messages; --,"
      "Bob <bob@example.test>,,,,50,51\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_by_from_addr_escapes_nullable_headers (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_with_escaped_nullable_headers (path);

  csv = dispatch_messages_by_from_addr_csv (path, "account-1",
      "Quote \" Sender <quote@example.test>");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-escaped,object-escaped,8,8,,"
      "\"Subject, with comma\","
      "\"Quote \"\" Sender <quote@example.test>\","
      "\"Line\nBreak <line@example.test>\",,,"
      "\"Tue, 02 Jan 2024 00:00:00 +0000\",30,31\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_returns_messages_by_sender_domain_csv (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_messages_by_sender_domain_headers (path);

  csv = dispatch_messages_by_sender_domain_csv (path, "account-1",
      "example.test");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-b,object-b,1,1,<message-b@example.test>,Subject B,"
      "Beta <beta@Example.TEST>,Bob <bob@example.test>,,,"
      "\"Sun, 31 Dec 2023 23:00:00 +0000\",18,19\n"
      "account-1,message-a,object-a,2,2,<message-a@example.test>,Subject A,"
      "Alice <alice@example.test>,Bob <bob@example.test>,"
      "Carol <carol@example.test>,Blind <blind@example.test>,"
      "\"Mon, 01 Jan 2024 00:00:00 +0000\",20,21\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_by_sender_domain_normalizes_parameter (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_messages_by_sender_domain_headers (path);

  csv = dispatch_messages_by_sender_domain_csv (path, "account-1",
      "Example.TEST");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-b,object-b,1,1,<message-b@example.test>,Subject B,"
      "Beta <beta@Example.TEST>,Bob <bob@example.test>,,,"
      "\"Sun, 31 Dec 2023 23:00:00 +0000\",18,19\n"
      "account-1,message-a,object-a,2,2,<message-a@example.test>,Subject A,"
      "Alice <alice@example.test>,Bob <bob@example.test>,"
      "Carol <carol@example.test>,Blind <blind@example.test>,"
      "\"Mon, 01 Jan 2024 00:00:00 +0000\",20,21\n");
  (void) g_remove (path);
}

static void
    test_duckdb_service_messages_by_sender_domain_isolates_cross_account_rows
    (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_messages_by_sender_domain_headers (path);

  csv = dispatch_messages_by_sender_domain_csv (path, "account-2",
      "example.test");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-2,message-other-account,object-other-account,5,5,"
      "<message-other-account@example.test>,Subject Other Account,"
      "Other <other@example.test>,Bob <bob@example.test>,,,"
      "\"Wed, 03 Jan 2024 00:00:00 +0000\",40,41\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_by_sender_domain_missing_is_header_only (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_messages_by_sender_domain_headers (path);

  csv = dispatch_messages_by_sender_domain_csv (path, "account-1",
      "missing.test");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_treats_sql_looking_sender_domain_as_value (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_sql_looking_sender_domain_message (path);

  csv = dispatch_messages_by_sender_domain_csv (path, "account-1",
      "domain'; DROP TABLE messages; --");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-sql-looking-domain,object-sql-looking-domain,"
      "12,12,,SQL Looking Domain,"
      "Sender <sender@domain'; drop table messages; -->,"
      "Bob <bob@example.test>,,,,70,71\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_returns_messages_by_subject_csv (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_header (path);

  csv = dispatch_messages_by_subject_csv (path, "account-1", "Subject A");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-a,object-a,2,2,<message-a@example.test>,Subject A,"
      "Alice <alice@example.test>,Bob <bob@example.test>,"
      "Carol <carol@example.test>,Blind <blind@example.test>,"
      "\"Mon, 01 Jan 2024 00:00:00 +0000\",20,21\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_by_subject_missing_subject_is_header_only (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_header (path);

  csv = dispatch_messages_by_subject_csv (path, "account-1", "subject a");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_by_subject_isolates_cross_account_rows (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_messages_by_from_addr_headers (path);

  csv = dispatch_messages_by_subject_csv (path, "account-2",
      "Subject Other Account");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-2,message-other-account,object-other-account,5,5,"
      "<message-other-account@example.test>,Subject Other Account,"
      "Alice <alice@example.test>,Bob <bob@example.test>,,,"
      "\"Wed, 03 Jan 2024 00:00:00 +0000\",40,41\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_by_subject_orders_multiple_rows (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_header (path);
  seed_messages_by_subject_headers (path);

  csv = dispatch_messages_by_subject_csv (path, "account-1", "Subject B");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-b,object-b,1,1,<message-b@example.test>,Subject B,"
      "Alice <alice@example.test>,Bob <bob@example.test>,,,"
      "\"Sun, 31 Dec 2023 23:00:00 +0000\",18,19\n"
      "account-1,message-hidden,object-hidden,3,3,"
      "<message-hidden@example.test>,Subject B,Zoe <zoe@example.test>,"
      "Bob <bob@example.test>,,,"
      "\"Thu, 04 Jan 2024 00:00:00 +0000\",42,43\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_by_subject_limits_high_cardinality_results (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_high_cardinality_subject_headers (path);

  csv = dispatch_messages_by_subject_csv (path, "account-1",
      "High Volume Subject");
  g_assert_cmpuint (count_substring (csv, "object-volume-"), ==, 100);
  g_assert_true (g_strstr_len (csv, -1, "message-volume-000") != NULL);
  g_assert_true (g_strstr_len (csv, -1, "message-volume-099") != NULL);
  g_assert_false (g_strstr_len (csv, -1, "message-volume-100") != NULL);
  g_assert_false (g_strstr_len (csv, -1, "message-volume-104") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_by_subject_page_csv (path, "account-1",
      "High Volume Subject", "5", "100");
  g_assert_cmpuint (count_substring (csv, "object-volume-"), ==, 5);
  g_assert_false (g_strstr_len (csv, -1, "message-volume-099") != NULL);
  g_assert_true (g_strstr_len (csv, -1, "message-volume-100") != NULL);
  g_assert_true (g_strstr_len (csv, -1, "message-volume-104") != NULL);
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_subject_contains_matches_case_insensitive (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_subject_contains_headers (path);

  csv = dispatch_messages_subject_contains_csv (path, "account-1", "SUBJECT",
      "100", "0");
  g_assert_true (g_strstr_len (csv, -1, "message-contains-a") != NULL);
  g_assert_true (g_strstr_len (csv, -1, "message-contains-b") != NULL);
  g_assert_false (g_strstr_len (csv, -1, "message-contains-null") != NULL);
  g_assert_false (g_strstr_len (csv, -1, "message-contains-other-account")
      != NULL);
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_subject_contains_keeps_non_ascii_literal (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_subject_contains_headers (path);

  csv = dispatch_messages_subject_contains_csv (path, "account-1", "résumé",
      "100", "0");
  g_assert_true (g_strstr_len (csv, -1, "message-contains-accented-latin")
      != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1", "RÉSUMÉ",
      "100", "0");
  g_assert_false (g_strstr_len (csv, -1, "message-contains-accented-latin")
      != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1",
      "συνάντηση", "100", "0");
  g_assert_true (g_strstr_len (csv, -1, "message-contains-greek") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1",
      "ΣΥΝΆΝΤΗΣΗ", "100", "0");
  g_assert_false (g_strstr_len (csv, -1, "message-contains-greek") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1", "istanbul",
      "100", "0");
  g_assert_true (g_strstr_len (csv, -1,
          "message-contains-turkish-dotted-i") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1",
      "İSTANBUL", "100", "0");
  g_assert_false (g_strstr_len (csv, -1,
          "message-contains-turkish-dotted-i") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1", "ışık",
      "100", "0");
  g_assert_true (g_strstr_len (csv, -1,
          "message-contains-turkish-dotless-i") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1", "IŞIK",
      "100", "0");
  g_assert_false (g_strstr_len (csv, -1,
          "message-contains-turkish-dotless-i") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1", "straße",
      "100", "0");
  g_assert_true (g_strstr_len (csv, -1, "message-contains-sharp-s") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1", "STRASSE",
      "100", "0");
  g_assert_false (g_strstr_len (csv, -1, "message-contains-sharp-s") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1", "café",
      "100", "0");
  g_assert_true (g_strstr_len (csv, -1,
          "message-contains-decoded-rfc2047") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1", "CAFé",
      "100", "0");
  g_assert_true (g_strstr_len (csv, -1,
          "message-contains-decoded-rfc2047") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1", "CAFÉ",
      "100", "0");
  g_assert_false (g_strstr_len (csv, -1,
          "message-contains-decoded-rfc2047") != NULL);

  (void) g_remove (path);
}

static void
    test_duckdb_service_messages_subject_contains_treats_wildcards_as_literals
    (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_subject_contains_headers (path);

  csv = dispatch_messages_subject_contains_csv (path, "account-1", "100%",
      "100", "0");
  g_assert_true (g_strstr_len (csv, -1, "message-contains-wildcard") != NULL);
  g_assert_false (g_strstr_len (csv, -1, "message-contains-a") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1", "build_",
      "100", "0");
  g_assert_true (g_strstr_len (csv, -1, "message-contains-underscore") != NULL);
  g_assert_false (g_strstr_len (csv, -1, "message-contains-a") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_subject_contains_csv (path, "account-1", "C:\\",
      "100", "0");
  g_assert_true (g_strstr_len (csv, -1, "message-contains-backslash") != NULL);
  g_assert_false (g_strstr_len (csv, -1, "message-contains-a") != NULL);
  (void) g_remove (path);
}

static void
    test_duckdb_service_messages_subject_contains_pages_high_cardinality_results
    (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_high_cardinality_subject_headers (path);

  csv = dispatch_messages_subject_contains_csv (path, "account-1", "volume",
      "10", "95");
  g_assert_cmpuint (count_substring (csv, "object-volume-"), ==, 10);
  g_assert_false (g_strstr_len (csv, -1, "message-volume-094") != NULL);
  g_assert_true (g_strstr_len (csv, -1, "message-volume-095") != NULL);
  g_assert_true (g_strstr_len (csv, -1, "message-volume-104") != NULL);
  (void) g_remove (path);
}

static void
test_duckdb_service_treats_sql_looking_subject_as_value (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_sql_looking_subject_message (path);

  csv = dispatch_messages_by_subject_csv (path, "account-1",
      "subject'; DROP TABLE messages; --");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-sql-looking-subject,object-sql-looking-subject,"
      "11,11,,subject'; DROP TABLE messages; --,"
      "Sender <sender@example.test>,Bob <bob@example.test>,,,,60,61\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_by_subject_escapes_nullable_headers (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_message_with_escaped_nullable_headers (path);

  csv = dispatch_messages_by_subject_csv (path, "account-1",
      "Subject, with comma");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-escaped,object-escaped,8,8,,"
      "\"Subject, with comma\","
      "\"Quote \"\" Sender <quote@example.test>\","
      "\"Line\nBreak <line@example.test>\",,,"
      "\"Tue, 02 Jan 2024 00:00:00 +0000\",30,31\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_returns_messages_by_date_range_csv (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_messages_by_date_range_headers (path);

  csv = dispatch_messages_by_date_range_csv (path, "account-1",
      "1704067200000000", "1704240000000000");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-a,object-a,2,2,<message-a@example.test>,Subject A,"
      "Alice <alice@example.test>,Bob <bob@example.test>,"
      "Carol <carol@example.test>,Blind <blind@example.test>,"
      "\"Mon, 01 Jan 2024 00:00:00 +0000\",20,21\n"
      "account-1,message-aa,object-aa,20,2,<message-aa@example.test>,"
      "Subject AA,Ann <ann@example.test>,Bob <bob@example.test>,,,"
      "\"Tue, 02 Jan 2024 00:00:00 +0000\",22,23\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_by_date_range_is_half_open (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_messages_by_date_range_headers (path);

  csv = dispatch_messages_by_date_range_csv (path, "account-1",
      "1704240000000000", "1704240000000001");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-1,message-hidden,object-hidden,3,3,"
      "<message-hidden@example.test>,Subject C,Zoe <zoe@example.test>,"
      "Bob <bob@example.test>,,,"
      "\"Wed, 03 Jan 2024 00:00:00 +0000\",42,43\n");
  (void) g_remove (path);
}

static void
    test_duckdb_service_messages_by_date_range_isolates_cross_account_rows
    (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_messages_by_date_range_headers (path);

  csv = dispatch_messages_by_date_range_csv (path, "account-2",
      "1704067200000000", "1704240000000000");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n"
      "account-2,message-other-account,object-other-account,5,5,"
      "<message-other-account@example.test>,Subject Other Account,"
      "Other <other@example.test>,Bob <bob@example.test>,,,"
      "\"Tue, 02 Jan 2024 00:00:00 +0000\",40,41\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_messages_by_date_range_excludes_null_dates (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_messages_by_date_range_headers (path);

  csv = dispatch_messages_by_date_range_csv (path, "account-1",
      "0", "9999999999999999");
  g_assert_false (g_strstr_len (csv, -1, "message-null-date") != NULL);
  g_assert_false (g_strstr_len (csv, -1, "Fri, 12 Jun 2026 10:00:00 EST")
      != NULL);
  (void) g_remove (path);
}

static void
test_duckdb_service_rejects_sql_looking_date_range_bound (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_messages_by_date_range_headers (path);

  assert_messages_by_date_range_rejects_bound (path, "account-1",
      "0); DROP TABLE messages; --", "9999999999999999");
  (void) g_remove (path);
}

static void
test_duckdb_service_accepts_date_range_int64_boundaries (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autofree gchar *csv = NULL;

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_messages_by_date_range_headers (path);

  csv = dispatch_messages_by_date_range_csv (path, "account-1",
      "-9223372036854775808", "9223372036854775807");
  g_assert_true (g_strstr_len (csv, -1, "message-a") != NULL);
  g_assert_true (g_strstr_len (csv, -1, "message-hidden") != NULL);
  g_assert_false (g_strstr_len (csv, -1, "message-null-date") != NULL);

  g_clear_pointer (&csv, g_free);
  csv = dispatch_messages_by_date_range_csv (path, "account-1", "0", "0");
  g_assert_cmpstr (csv, ==,
      "account_id,message_id,object_id,message_journal_offset,"
      "message_journal_sequence,rfc_message_id,subject,from_addr,to_addr,"
      "cc_addr,bcc_addr,date_raw,header_journal_offset,"
      "header_journal_sequence\n");
  (void) g_remove (path);
}

static void
test_duckdb_service_rejects_overflowing_date_range_bound (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();

  bootstrap_catalog (path);
  seed_catalog (path);
  seed_messages_by_date_range_headers (path);

  assert_messages_by_date_range_rejects_bound (path, "account-1",
      "9223372036854775808", "9223372036854775807");
  assert_messages_by_date_range_rejects_bound (path, "account-1",
      "-9223372036854775809", "0");
  (void) g_remove (path);
}

static void
test_duckdb_service_missing_path_fails (void)
{
  g_autofree gchar *path = create_temp_catalog_path ();
  g_autoptr (GError) error = NULL;
  g_autoptr (WyreboxDaemonDuckDBQueryTemplateService) service = NULL;

  service = wyrebox_daemon_duckdb_query_template_service_new_duckdb (path,
      &error);
  g_assert_null (service);
  g_assert_error (error, G_IO_ERROR, G_IO_ERROR_FAILED);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/derived-view-uid-map-csv",
      test_duckdb_service_returns_derived_view_uid_map_csv);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/derived-view-empty-result-header-only",
      test_duckdb_service_derived_view_empty_result_is_header_only);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/derived-view-cross-account-isolation",
      test_duckdb_service_derived_view_isolates_cross_account_rows);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/sql-looking-derived-view-value",
      test_duckdb_service_treats_sql_looking_derived_view_as_value);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/returns-uid-map-csv",
      test_duckdb_service_returns_uid_map_csv);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/empty-result-header-only",
      test_duckdb_service_empty_result_is_header_only);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/cross-account-isolation",
      test_duckdb_service_isolates_cross_account_rows);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/sql-looking-mailbox-value",
      test_duckdb_service_treats_sql_looking_mailbox_as_value);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/message-by-id-with-headers",
      test_duckdb_service_returns_message_by_id_with_headers);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/message-by-id-cross-account-isolation",
      test_duckdb_service_message_by_id_isolates_cross_account_rows);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/message-by-id-missing-message",
      test_duckdb_service_message_by_id_missing_message_is_header_only);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/sql-looking-message-id-value",
      test_duckdb_service_treats_sql_looking_message_id_as_value);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/message-by-id-escaped-nullable-headers",
      test_duckdb_service_message_by_id_escapes_nullable_headers);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/message-facts-by-message-id-csv",
      test_duckdb_service_returns_message_facts_by_message_id_csv);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/message-facts-by-message-id-cross-account-isolation",
      test_duckdb_service_message_facts_by_message_id_isolates_cross_account_rows);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/message-facts-by-message-id-missing-message",
      test_duckdb_service_message_facts_by_message_id_missing_message_is_header_only);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/facts-by-source-csv",
      test_duckdb_service_returns_facts_by_source_csv);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/facts-by-source-cross-account-isolation",
      test_duckdb_service_facts_by_source_isolates_cross_account_rows);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/facts-by-source-missing-source",
      test_duckdb_service_facts_by_source_missing_source_is_header_only);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/facts-by-fact-id-csv",
      test_duckdb_service_returns_facts_by_fact_id_csv);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/facts-by-fact-id-with-provenance-csv",
      test_duckdb_service_returns_facts_by_fact_id_with_provenance_csv);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/facts-by-fact-id-cross-account-isolation",
      test_duckdb_service_facts_by_fact_id_isolates_cross_account_rows);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/facts-by-fact-id-missing-fact",
      test_duckdb_service_facts_by_fact_id_missing_fact_is_header_only);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-from-addr-csv",
      test_duckdb_service_returns_messages_by_from_addr_csv);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-from-addr-missing-sender-header-only",
      test_duckdb_service_messages_by_from_addr_missing_sender_is_header_only);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-from-addr-cross-account-isolation",
      test_duckdb_service_messages_by_from_addr_isolates_cross_account_rows);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-from-addr-ordering",
      test_duckdb_service_messages_by_from_addr_orders_multiple_rows);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/sql-looking-from-addr-value",
      test_duckdb_service_treats_sql_looking_from_addr_as_value);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-from-addr-escaped-nullable-headers",
      test_duckdb_service_messages_by_from_addr_escapes_nullable_headers);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-sender-domain-csv",
      test_duckdb_service_returns_messages_by_sender_domain_csv);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-sender-domain-normalizes-parameter",
      test_duckdb_service_messages_by_sender_domain_normalizes_parameter);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-sender-domain-cross-account-isolation",
      test_duckdb_service_messages_by_sender_domain_isolates_cross_account_rows);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-sender-domain-missing-header-only",
      test_duckdb_service_messages_by_sender_domain_missing_is_header_only);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/sql-looking-sender-domain-value",
      test_duckdb_service_treats_sql_looking_sender_domain_as_value);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-subject-csv",
      test_duckdb_service_returns_messages_by_subject_csv);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-subject-missing-subject-header-only",
      test_duckdb_service_messages_by_subject_missing_subject_is_header_only);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-subject-cross-account-isolation",
      test_duckdb_service_messages_by_subject_isolates_cross_account_rows);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-subject-ordering",
      test_duckdb_service_messages_by_subject_orders_multiple_rows);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-subject-high-cardinality-limit",
      test_duckdb_service_messages_by_subject_limits_high_cardinality_results);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-subject-contains-case-insensitive",
      test_duckdb_service_messages_subject_contains_matches_case_insensitive);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-subject-contains-non-ascii-literal",
      test_duckdb_service_messages_subject_contains_keeps_non_ascii_literal);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-subject-contains-literal-wildcards",
      test_duckdb_service_messages_subject_contains_treats_wildcards_as_literals);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-subject-contains-high-cardinality-page",
      test_duckdb_service_messages_subject_contains_pages_high_cardinality_results);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/sql-looking-subject-value",
      test_duckdb_service_treats_sql_looking_subject_as_value);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-subject-escaped-nullable-headers",
      test_duckdb_service_messages_by_subject_escapes_nullable_headers);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-date-range-csv",
      test_duckdb_service_returns_messages_by_date_range_csv);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-date-range-half-open",
      test_duckdb_service_messages_by_date_range_is_half_open);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-date-range-cross-account-isolation",
      test_duckdb_service_messages_by_date_range_isolates_cross_account_rows);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/messages-by-date-range-null-exclusion",
      test_duckdb_service_messages_by_date_range_excludes_null_dates);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/sql-looking-date-range-bound-rejected",
      test_duckdb_service_rejects_sql_looking_date_range_bound);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/date-range-int64-boundaries",
      test_duckdb_service_accepts_date_range_int64_boundaries);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/date-range-overflow-rejected",
      test_duckdb_service_rejects_overflowing_date_range_bound);
  g_test_add_func
      ("/daemon-api/duckdb-query-template/service-duckdb/missing-path-fails",
      test_duckdb_service_missing_path_fails);

  return g_test_run ();
}
