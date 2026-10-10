#include "wyrebox-daemon-fact-extraction.h"

#include "wyrebox-duckdb-shared.h"
#include "wyrebox-eml-metadata.h"
#include "wyrebox-facts-extracted-payload.h"

#include <duckdb.h>
#include <gio/gio.h>

struct _WyreboxDaemonFactExtraction
{
    GObject parent_instance;

    WyreboxFactExtractionRules *rules;
    WyreboxLocalObjectStore *object_store;
    WyreboxJournalWriter *journal_writer;
    duckdb_database database;
    duckdb_connection connection;
};

G_DEFINE_TYPE (WyreboxDaemonFactExtraction, wyrebox_daemon_fact_extraction,
    G_TYPE_OBJECT);

static void
duckdb_result_clear (duckdb_result *result)
{
    duckdb_destroy_result (result);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_result, duckdb_result_clear)
/* *INDENT-ON* */

static void
duckdb_prepared_statement_clear (duckdb_prepared_statement *statement)
{
    duckdb_destroy_prepare (statement);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_prepared_statement,
    duckdb_prepared_statement_clear)
/* *INDENT-ON* */

static void
wyrebox_daemon_fact_extraction_dispose (GObject *object)
{
    WyreboxDaemonFactExtraction *self = WYREBOX_DAEMON_FACT_EXTRACTION (object);

    g_clear_object (&self->rules);
    g_clear_object (&self->object_store);
    g_clear_object (&self->journal_writer);

    G_OBJECT_CLASS (wyrebox_daemon_fact_extraction_parent_class)->dispose
        (object);
}

static void
wyrebox_daemon_fact_extraction_finalize (GObject *object)
{
    WyreboxDaemonFactExtraction *self = WYREBOX_DAEMON_FACT_EXTRACTION (object);

    if (self->connection != NULL)
        duckdb_disconnect (&self->connection);
    if (self->database != NULL)
        duckdb_close (&self->database);

    G_OBJECT_CLASS (wyrebox_daemon_fact_extraction_parent_class)->finalize
        (object);
}

static void
wyrebox_daemon_fact_extraction_class_init (WyreboxDaemonFactExtractionClass
    *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = wyrebox_daemon_fact_extraction_dispose;
    object_class->finalize = wyrebox_daemon_fact_extraction_finalize;
}

static void
wyrebox_daemon_fact_extraction_init (WyreboxDaemonFactExtraction *self)
{
}

WyreboxDaemonFactExtraction *
wyrebox_daemon_fact_extraction_new (const char *catalog_path,
    WyreboxFactExtractionRules *rules, WyreboxLocalObjectStore *object_store,
    WyreboxJournalWriter *journal_writer, GError **error)
{
    g_autoptr (WyreboxDaemonFactExtraction) self = NULL;

    g_return_val_if_fail (catalog_path != NULL, NULL);
    g_return_val_if_fail (WYREBOX_IS_FACT_EXTRACTION_RULES (rules), NULL);
    g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (object_store), NULL);
    g_return_val_if_fail (WYREBOX_IS_JOURNAL_WRITER (journal_writer), NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    self = g_object_new (WYREBOX_TYPE_DAEMON_FACT_EXTRACTION, NULL);
    self->rules = g_object_ref (rules);
    self->object_store = g_object_ref (object_store);
    self->journal_writer = g_object_ref (journal_writer);

    if (!wyrebox_duckdb_open_shared (catalog_path, &self->database, error))
        return NULL;

    if (duckdb_connect (self->database, &self->connection) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "failed to connect to DuckDB catalog '%s'", catalog_path);
        return NULL;
    }

    return g_steal_pointer (&self);
}

static char *
result_string (duckdb_result *result, idx_t column, idx_t row)
{
    char *value = duckdb_value_varchar (result, column, row);
    char *copy = g_strdup (value != NULL ? value : "");

    duckdb_free (value);
    return copy;
}

/*
 * Returns: (transfer full): pairs of message id and object id, in journal
 * order, or NULL with @error set.
 */
static GPtrArray *
list_pending_messages (WyreboxDaemonFactExtraction *self,
    const char *account_id, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    g_autoptr (GPtrArray) messages = g_ptr_array_new_with_free_func (g_free);

    if (duckdb_prepare (self->connection,
        "SELECT m.message_id, m.object_id FROM messages m "
        "WHERE m.account_id = ? AND NOT EXISTS ("
        "SELECT 1 FROM message_fact_extractions e "
        "WHERE e.account_id = m.account_id AND e.message_id = m.message_id) "
        "ORDER BY m.journal_sequence;", &statement) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 1, account_id) != DuckDBSuccess ||
        duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        const char *detail = statement != NULL ?
            duckdb_prepare_error (statement) : NULL;

        if (detail == NULL)
            detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "failed to list messages pending fact extraction: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return NULL;
    }

    for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
        g_ptr_array_add (messages, result_string (&result, 0, row));
        g_ptr_array_add (messages, result_string (&result, 1, row));
    }

    return g_steal_pointer (&messages);
}

static void
make_valid (char **value)
{
    char *valid = NULL;

    if (*value == NULL)
        return;

    valid = g_utf8_make_valid (*value, -1);
    g_free (*value);
    *value = valid;
}

static gboolean
extract_message (WyreboxDaemonFactExtraction *self, const char *account_id,
    const char *message_id, const char *object_id, GError **error)
{
    g_auto (WyreboxEmlMetadata) metadata = { 0 };
    g_auto (WyreboxFactsExtractedPayload) payload = { 0 };
    g_autoptr (GBytes) raw = NULL;
    g_autoptr (GBytes) encoded = NULL;
    guint64 offset = 0;
    guint64 sequence = 0;

    raw = wyrebox_local_object_store_get_bytes (self->object_store, object_id,
            error);
    if (raw == NULL ||
        !wyrebox_eml_metadata_parse_bytes (raw, &metadata, error))
        return FALSE;

    make_valid (&metadata.message_id);
    make_valid (&metadata.subject);
    make_valid (&metadata.from);
    make_valid (&metadata.to);
    make_valid (&metadata.cc);
    make_valid (&metadata.bcc);
    make_valid (&metadata.date);
    make_valid (&metadata.in_reply_to);
    make_valid (&metadata.references);

    payload.account_id = g_strdup (account_id);
    payload.message_id = g_strdup (message_id);
    payload.extracted_at_unix_us = (guint64)MAX (g_get_real_time (), 1);
    payload.facts = wyrebox_fact_extraction_rules_extract (self->rules,
            message_id, &metadata, payload.extracted_at_unix_us, error);
    if (payload.facts == NULL)
        return FALSE;
    g_ptr_array_set_free_func (payload.facts,
        wyrebox_facts_extracted_payload_fact_free);

    encoded = wyrebox_facts_extracted_payload_encode (&payload, error);
    return encoded != NULL &&
           wyrebox_journal_writer_append (self->journal_writer,
               WYREBOX_JOURNAL_EVENT_FACTS_EXTRACTED, encoded, &offset,
               &sequence, error);
}

gboolean
wyrebox_daemon_fact_extraction_extract_account (WyreboxDaemonFactExtraction
    *self, const char *account_id, guint *out_appended, GError **error)
{
    g_autoptr (GPtrArray) messages = NULL;

    g_return_val_if_fail (WYREBOX_IS_DAEMON_FACT_EXTRACTION (self), FALSE);
    g_return_val_if_fail (account_id != NULL, FALSE);
    g_return_val_if_fail (out_appended != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    *out_appended = 0;
    messages = list_pending_messages (self, account_id, error);
    if (messages == NULL)
        return FALSE;

    for (guint i = 0; i + 1 < messages->len; i += 2) {
        const char *message_id = g_ptr_array_index (messages, i);

        if (!extract_message (self, account_id, message_id,
            g_ptr_array_index (messages, i + 1), error)) {
            g_prefix_error (error, "message %s: ", message_id);
            return FALSE;
        }
        (*out_appended)++;
    }

    return TRUE;
}
