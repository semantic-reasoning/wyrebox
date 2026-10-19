#include "wyrebox-daemon-message-append-journal.h"

#include "wyrebox-duckdb-shared.h"
#include "wyrebox-eml-metadata.h"
#include "wyrebox-message-appended-payload.h"

#include <duckdb.h>
#include <gio/gio.h>

#define APPEND_MAILBOX_UIDVALIDITY 1

typedef struct
{
    GMutex connection_mutex;
    duckdb_database database;
    duckdb_connection connection;
    WyreboxJournalWriter *journal_writer;
    WyreboxLocalObjectStore *object_store;
    WyreboxDaemonDeliveryMaterialization *materialization;
} MessageAppendJournal;

typedef struct
{
    MessageAppendJournal *self;
    const WyreboxDaemonMessageAppendRequest *request;
    const WyreboxEmlMetadata *metadata;
    guint64 reserved_uid;
} AppendGuardedContext;

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
message_append_journal_free (gpointer data)
{
    MessageAppendJournal *self = data;

    if (self->connection != NULL)
        duckdb_disconnect (&self->connection);
    if (self->database != NULL)
        duckdb_close (&self->database);
    g_clear_object (&self->journal_writer);
    g_clear_object (&self->object_store);
    g_clear_object (&self->materialization);
    g_mutex_clear (&self->connection_mutex);
    g_free (self);
}

/* *INDENT-OFF* */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MessageAppendJournal, message_append_journal_free)
/* *INDENT-ON* */

static gboolean
lookup_derived_view (MessageAppendJournal *self, const char *account_id,
    const char *view_id, gboolean *out_is_derived_view, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    const char *detail = NULL;

    if (duckdb_prepare (self->connection,
        "SELECT 1 FROM derived_views "
        "WHERE account_id = ? AND view_id = ? AND is_visible = TRUE;",
        &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append derived view prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1, account_id) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2, view_id) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "message append bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append derived view lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    *out_is_derived_view = duckdb_row_count (&result) > 0;
    return TRUE;
}

static gboolean
ensure_uid_state_validity (MessageAppendJournal *self,
    const WyreboxDaemonMessageAppendRequest *request, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    guint64 count = 0;
    const char *detail = NULL;

    if (duckdb_prepare (self->connection,
        "INSERT OR IGNORE INTO mailbox_uid_state ("
        "account_id, namespace_kind, namespace_id, uidnext, uidvalidity"
        ") VALUES (?, 'mailbox', ?, 1, ?);",
        &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append uid state prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->mailbox_id) != DuckDBSuccess ||
        duckdb_bind_uint64 (statement, 3,
        request->uid_validity) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "message append bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append uid state insert failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    duckdb_destroy_result (&result);
    duckdb_destroy_prepare (&statement);
    if (duckdb_prepare (self->connection,
        "SELECT COUNT(*) FROM mailbox_uid_state "
        "WHERE account_id = ? AND namespace_kind = 'mailbox' "
        "AND namespace_id = ? AND uidvalidity = ?;",
        &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append uid state verify prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->mailbox_id) != DuckDBSuccess ||
        duckdb_bind_uint64 (statement, 3,
        request->uid_validity) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "message append bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append uid state verify failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    count = (guint64)duckdb_value_uint64 (&result, 0, 0);
    if (count != 1) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_EXISTS,
            "UIDVALIDITY mismatch for mailbox %s", request->mailbox_id);
        return FALSE;
    }

    return TRUE;
}

static gboolean
check_materialized_mailbox (MessageAppendJournal *self,
    const WyreboxDaemonMessageAppendRequest *request, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    const char *detail = NULL;

    if (duckdb_prepare (self->connection,
        "SELECT us.uidvalidity FROM mailbox_uid_state us "
        "JOIN mailboxes mb ON mb.account_id = us.account_id "
        "AND mb.mailbox_id = us.namespace_id "
        "WHERE us.account_id = ? AND us.namespace_kind = 'mailbox' "
        "AND us.namespace_id = ? "
        "AND mb.is_visible = TRUE AND mb.is_selectable = TRUE;",
        &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append destination prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->mailbox_id) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "message append bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append destination lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_row_count (&result) == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "selectable mailbox %s was not found for account %s",
            request->mailbox_id, request->account_identity);
        return FALSE;
    }

    if ((guint64)duckdb_value_uint64 (&result, 0,
        0) != request->uid_validity) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_EXISTS,
            "UIDVALIDITY mismatch for mailbox %s", request->mailbox_id);
        return FALSE;
    }

    return TRUE;
}

static gboolean
check_append_destination (MessageAppendJournal *self,
    const WyreboxDaemonMessageAppendRequest *request, GError **error)
{
    g_autofree char *expected_inbox_id = NULL;
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    gboolean is_derived_view = FALSE;
    guint64 mailbox_count = 0;
    const char *detail = NULL;

    if (!lookup_derived_view (self, request->account_identity,
        request->mailbox_id, &is_derived_view, error))
        return FALSE;

    if (is_derived_view) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_PERMISSION_DENIED,
            "append destination is not permitted for virtual mailbox %s",
            request->mailbox_id);
        return FALSE;
    }

    if (duckdb_prepare (self->connection,
        "SELECT COUNT(*) FROM mailboxes WHERE mailbox_id = ? "
        "AND account_id = ? AND is_selectable = TRUE AND is_visible = TRUE;",
        &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append mailbox prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1, request->mailbox_id) != DuckDBSuccess
        || duckdb_bind_varchar (statement, 2,
        request->account_identity) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "message append bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append mailbox lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    mailbox_count = (guint64)duckdb_value_uint64 (&result, 0, 0);
    if (mailbox_count == 1)
        return check_materialized_mailbox (self, request, error);

    expected_inbox_id = g_strdup_printf ("inbox:%s",
            request->account_identity);
    if (g_strcmp0 (request->mailbox_id, expected_inbox_id) != 0 ||
        request->uid_validity != APPEND_MAILBOX_UIDVALIDITY) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "selectable mailbox %s was not found for account %s",
            request->mailbox_id, request->account_identity);
        return FALSE;
    }

    return ensure_uid_state_validity (self, request, error);
}

static gboolean
reserve_destination_uid (MessageAppendJournal *self,
    const WyreboxDaemonMessageAppendRequest *request, guint64 *out_uid,
    GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    const char *detail = NULL;
    guint64 uidnext = 0;

    if (!ensure_uid_state_validity (self, request, error))
        return FALSE;

    if (duckdb_prepare (self->connection,
        "SELECT uidnext FROM mailbox_uid_state "
        "WHERE account_id = ? AND namespace_kind = 'mailbox' "
        "AND namespace_id = ? AND uidvalidity = ?;",
        &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append uidnext prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->mailbox_id) != DuckDBSuccess ||
        duckdb_bind_uint64 (statement, 3,
        request->uid_validity) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "message append bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append uidnext lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_row_count (&result) != 1) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "destination mailbox %s uid state was not found",
            request->mailbox_id);
        return FALSE;
    }

    uidnext = (guint64)duckdb_value_uint64 (&result, 0, 0);
    if (uidnext == 0 || uidnext == G_MAXUINT64) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "destination mailbox %s uidnext is invalid",
            request->mailbox_id);
        return FALSE;
    }

    duckdb_destroy_result (&result);
    duckdb_destroy_prepare (&statement);
    if (duckdb_prepare (self->connection,
        "UPDATE mailbox_uid_state SET uidnext = ? "
        "WHERE account_id = ? AND namespace_kind = 'mailbox' "
        "AND namespace_id = ? AND uidvalidity = ?;",
        &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append uidnext update prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_uint64 (statement, 1, uidnext + 1) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 3,
        request->mailbox_id) != DuckDBSuccess ||
        duckdb_bind_uint64 (statement, 4,
        request->uid_validity) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "message append bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append uidnext update failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    *out_uid = uidnext;
    return TRUE;
}

static gboolean
append_object_and_payload_locked (const char *journal_root_dir,
    gpointer user_data, GBytes **out_payload, guint64 *out_offset,
    guint64 *out_sequence, GError **error)
{
    AppendGuardedContext *context = user_data;
    const WyreboxDaemonMessageAppendRequest *request = context->request;
    g_autofree char *stored_object_key = NULL;
    g_auto (WyreboxMessageAppendedPayload) payload = { 0 };
    g_autoptr (GBytes) encoded = NULL;

    (void)journal_root_dir;
    (void)out_offset;
    (void)out_sequence;

    if (!wyrebox_local_object_store_put_bytes (context->self->object_store,
        request->message_bytes, &stored_object_key, error))
        return FALSE;

    payload.account_id = g_strdup (request->account_identity);
    payload.mailbox_id = g_strdup (request->mailbox_id);
    payload.uidvalidity = request->uid_validity;
    payload.uid = context->reserved_uid;
    payload.object_key = g_steal_pointer (&stored_object_key);
    payload.size_bytes = (guint64)g_bytes_get_size (request->message_bytes);
    payload.internal_date_unix_us = request->internal_date_unix_us;
    payload.message_id = g_strdup (context->metadata->message_id);
    payload.subject = g_strdup (context->metadata->subject);
    payload.from = g_strdup (context->metadata->from);
    payload.to = g_strdup (context->metadata->to);
    payload.cc = g_strdup (context->metadata->cc);
    payload.bcc = g_strdup (context->metadata->bcc);
    payload.date = g_strdup (context->metadata->date);
    payload.duplicate_message_id_count =
        context->metadata->duplicate_message_id_count;
    payload.message_id_span_valid = context->metadata->message_id_span_valid;
    payload.message_id_span_start = context->metadata->message_id_span_start;
    payload.message_id_span_end = context->metadata->message_id_span_end;
    payload.subject_span_valid = context->metadata->subject_span_valid;
    payload.subject_span_start = context->metadata->subject_span_start;
    payload.subject_span_end = context->metadata->subject_span_end;
    payload.system_flags = g_strdupv (request->system_flags);
    payload.user_keywords = g_strdupv (request->user_keywords);

    encoded = wyrebox_message_appended_payload_encode (&payload, error);
    if (encoded == NULL)
        return FALSE;

    *out_payload = g_steal_pointer (&encoded);
    return TRUE;
}

static char *
format_summary (const WyreboxDaemonMessageAppendRequest *request,
    guint64 assigned_uid)
{
    return g_strdup_printf ("message_appended dest_mailbox_id=%s "
               "dest_uidvalidity=%" G_GUINT64_FORMAT " pairs=%" G_GUINT64_FORMAT
               ":%" G_GUINT64_FORMAT,
               request->mailbox_id, request->uid_validity,
               request->uid_validity,
               assigned_uid);
}

static gboolean
append_message (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageAppendRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt, gpointer user_data,
    GError **error)
{
    MessageAppendJournal *self = user_data;
    AppendGuardedContext guarded_context = { 0 };
    g_auto (WyreboxEmlMetadata) metadata = { 0 };
    guint64 journal_offset = 0;
    guint64 journal_sequence = 0;
    guint64 reserved_uid = 0;

    if (!wyrebox_eml_metadata_parse_bytes (request->message_bytes, &metadata,
        error))
        return FALSE;

    if (!wyrebox_daemon_delivery_materialization_catch_up (
            self->materialization,
            error))
        return FALSE;

    {
        g_autoptr (GMutexLocker) locker =
            g_mutex_locker_new (&self->connection_mutex);

        if (!check_append_destination (self, request, error))
            return FALSE;

        if (!reserve_destination_uid (self, request, &reserved_uid, error))
            return FALSE;
    }

    guarded_context.self = self;
    guarded_context.request = request;
    guarded_context.metadata = &metadata;
    guarded_context.reserved_uid = reserved_uid;

    if (!wyrebox_journal_writer_append_guarded (self->journal_writer,
        WYREBOX_JOURNAL_EVENT_MESSAGE_APPENDED,
        append_object_and_payload_locked, &guarded_context, &journal_offset,
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
    out_receipt->summary = format_summary (request, reserved_uid);
    return TRUE;
}

WyreboxDaemonMessageAppendService *
wyrebox_daemon_message_append_service_new_journaled (const char *catalog_path,
    WyreboxJournalWriter *journal_writer,
    WyreboxLocalObjectStore *object_store,
    WyreboxDaemonDeliveryMaterialization *materialization, GError **error)
{
    g_autoptr (MessageAppendJournal) self = NULL;

    g_return_val_if_fail (catalog_path != NULL, NULL);
    g_return_val_if_fail (WYREBOX_IS_JOURNAL_WRITER (journal_writer), NULL);
    g_return_val_if_fail (WYREBOX_IS_LOCAL_OBJECT_STORE (object_store), NULL);
    g_return_val_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION
            (materialization), NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    self = g_new0 (MessageAppendJournal, 1);
    g_mutex_init (&self->connection_mutex);
    self->journal_writer = g_object_ref (journal_writer);
    self->object_store = g_object_ref (object_store);
    self->materialization = g_object_ref (materialization);

    if (!wyrebox_duckdb_open_shared (catalog_path, &self->database, error))
        return NULL;

    if (duckdb_connect (self->database, &self->connection) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message append connect failed for '%s'", catalog_path);
        return NULL;
    }

    return wyrebox_daemon_message_append_service_new (append_message,
               g_steal_pointer (&self), message_append_journal_free);
}
