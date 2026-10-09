#include "wyrebox-daemon-flag-keyword-update-journal.h"

#include "wyrebox-duckdb-shared.h"
#include "wyrebox-flag-changed-payload.h"

#include <duckdb.h>
#include <gio/gio.h>

typedef struct
{
    GMutex connection_mutex;
    duckdb_database database;
    duckdb_connection connection;
    WyreboxJournalWriter *journal_writer;
    WyreboxDaemonDeliveryMaterialization *materialization;
} FlagKeywordUpdateJournal;

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
flag_keyword_update_journal_free (gpointer data)
{
    FlagKeywordUpdateJournal *self = data;

    if (self->connection != NULL)
        duckdb_disconnect (&self->connection);
    if (self->database != NULL)
        duckdb_close (&self->database);
    g_clear_object (&self->journal_writer);
    g_clear_object (&self->materialization);
    g_mutex_clear (&self->connection_mutex);
    g_free (self);
}

/* *INDENT-OFF* */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (FlagKeywordUpdateJournal,
    flag_keyword_update_journal_free)
/* *INDENT-ON* */

static gboolean
query_target (FlagKeywordUpdateJournal *self, const char *sql,
    const WyreboxDaemonFlagKeywordUpdateRequest *request, gboolean bind_uid,
    duckdb_result *out_result, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    const char *detail = NULL;

    if (duckdb_prepare (self->connection, sql, &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "flag keyword update prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->mailbox_id) != DuckDBSuccess ||
        (bind_uid && duckdb_bind_uint64 (statement, 3,
        request->mailbox_uid) != DuckDBSuccess)) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "flag keyword update bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, out_result) == DuckDBSuccess)
        return TRUE;

    detail = duckdb_result_error (out_result);
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "flag keyword update target lookup failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
}

static gboolean
check_target (FlagKeywordUpdateJournal *self,
    const WyreboxDaemonFlagKeywordUpdateRequest *request, GError **error)
{
    g_auto (duckdb_result) mailbox = { 0 };
    g_auto (duckdb_result) membership = { 0 };

    if (!query_target (self,
        "SELECT us.uidvalidity FROM mailbox_uid_state us "
        "JOIN mailboxes mb ON mb.account_id = us.account_id "
        "AND mb.mailbox_id = us.namespace_id "
        "WHERE us.account_id = ? AND us.namespace_kind = 'mailbox' "
        "AND us.namespace_id = ? "
        "AND mb.is_visible = TRUE AND mb.is_selectable = TRUE;",
        request, FALSE, &mailbox, error))
        return FALSE;

    if (duckdb_row_count (&mailbox) == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "selectable mailbox %s was not found for account %s",
            request->mailbox_id, request->account_identity);
        return FALSE;
    }

    if ((guint64)duckdb_value_uint64 (&mailbox, 0,
        0) != request->uid_validity) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_EXISTS,
            "UIDVALIDITY mismatch for mailbox %s", request->mailbox_id);
        return FALSE;
    }

    if (!query_target (self,
        "SELECT 1 FROM mailbox_memberships "
        "WHERE account_id = ? AND mailbox_id = ? AND uid = ? "
        "AND is_visible = TRUE;", request, TRUE, &membership, error))
        return FALSE;

    if (duckdb_row_count (&membership) == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "UID %" G_GUINT64_FORMAT " was not found in mailbox %s",
            request->mailbox_uid, request->mailbox_id);
        return FALSE;
    }

    return TRUE;
}

static WyreboxFlagChangedMode
flag_changed_mode (WyreboxDaemonFlagKeywordUpdateMode mode)
{
    switch (mode) {
    case WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR:
        return WYREBOX_FLAG_CHANGED_MODE_CLEAR;
    case WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE:
        return WYREBOX_FLAG_CHANGED_MODE_REPLACE;
    case WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET:
    default:
        return WYREBOX_FLAG_CHANGED_MODE_SET;
    }
}

static const char *
mode_name (WyreboxDaemonFlagKeywordUpdateMode mode)
{
    switch (mode) {
    case WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR:
        return "clear";
    case WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_REPLACE:
        return "replace";
    case WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET:
    default:
        return "set";
    }
}

static gboolean
append_flag_change (FlagKeywordUpdateJournal *self,
    const WyreboxDaemonFlagKeywordUpdateRequest *request,
    guint64 *out_offset, guint64 *out_sequence, GError **error)
{
    WyreboxFlagChangedPayload payload = {
        .account_id = request->account_identity,
        .mailbox_id = request->mailbox_id,
        .uidvalidity = request->uid_validity,
        .uid = request->mailbox_uid,
        .mode = flag_changed_mode (request->mode),
        .system_flags = request->system_flags,
        .user_keywords = request->user_keywords,
    };
    g_autoptr (GBytes) bytes = NULL;

    bytes = wyrebox_flag_changed_payload_encode (&payload, error);
    if (bytes == NULL)
        return FALSE;

    return wyrebox_journal_writer_append (self->journal_writer,
               WYREBOX_JOURNAL_EVENT_FLAG_CHANGED, bytes, out_offset,
               out_sequence, error);
}

static gboolean
update_flags (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonFlagKeywordUpdateRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt, gpointer user_data,
    GError **error)
{
    FlagKeywordUpdateJournal *self = user_data;
    guint64 journal_offset = 0;
    guint64 journal_sequence = 0;

    {
        g_autoptr (GMutexLocker) locker =
            g_mutex_locker_new (&self->connection_mutex);

        if (!check_target (self, request, error))
            return FALSE;
    }

    if (!append_flag_change (self, request, &journal_offset,
        &journal_sequence, error))
        return FALSE;

    wyrebox_daemon_delivery_materialization_catch_up_or_schedule_retry
        (self->materialization);

    wyrebox_daemon_success_receipt_clear (out_receipt);
    out_receipt->request_id = g_strdup (identity->request_id);
    out_receipt->durable_marker = g_strdup_printf ("journal:%"
            G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT, journal_offset,
            journal_sequence);
    out_receipt->journal_offset = journal_offset;
    out_receipt->journal_sequence = journal_sequence;
    out_receipt->summary = g_strdup_printf ("flag_keyword_update mode=%s "
            "mailbox_id=%s uid=%" G_GUINT64_FORMAT, mode_name (request->mode),
            request->mailbox_id, request->mailbox_uid);
    return TRUE;
}

WyreboxDaemonFlagKeywordUpdateService *
wyrebox_daemon_flag_keyword_update_service_new_journaled (const char
    *catalog_path, WyreboxJournalWriter *journal_writer,
    WyreboxDaemonDeliveryMaterialization *materialization, GError **error)
{
    g_autoptr (FlagKeywordUpdateJournal) self = NULL;

    g_return_val_if_fail (catalog_path != NULL, NULL);
    g_return_val_if_fail (WYREBOX_IS_JOURNAL_WRITER (journal_writer), NULL);
    g_return_val_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION
            (materialization), NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    self = g_new0 (FlagKeywordUpdateJournal, 1);
    g_mutex_init (&self->connection_mutex);
    self->journal_writer = g_object_ref (journal_writer);
    self->materialization = g_object_ref (materialization);

    if (!wyrebox_duckdb_open_shared (catalog_path, &self->database, error))
        return NULL;

    if (duckdb_connect (self->database, &self->connection) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "flag keyword update connect failed for '%s'", catalog_path);
        return NULL;
    }

    return wyrebox_daemon_flag_keyword_update_service_new (update_flags,
               g_steal_pointer (&self), flag_keyword_update_journal_free);
}
