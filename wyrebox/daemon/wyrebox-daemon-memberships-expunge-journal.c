#include "wyrebox-daemon-memberships-expunge-journal.h"

#include "wyrebox-duckdb-shared.h"
#include "wyrebox-memberships-expunged-payload.h"

#include <duckdb.h>
#include <gio/gio.h>

typedef struct
{
    GMutex connection_mutex;
    duckdb_database database;
    duckdb_connection connection;
    WyreboxJournalWriter *journal_writer;
    WyreboxDaemonDeliveryMaterialization *materialization;
} MembershipsExpungeJournal;

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
memberships_expunge_journal_free (gpointer data)
{
    MembershipsExpungeJournal *self = data;

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
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MembershipsExpungeJournal,
    memberships_expunge_journal_free)
/* *INDENT-ON* */

static gboolean
load_materialization_checkpoint (MembershipsExpungeJournal *self,
    guint64 *out_offset, guint64 *out_sequence, GError **error)
{
    g_auto (duckdb_result) result = { 0 };
    const char *detail = NULL;

    if (duckdb_query (self->connection,
        "SELECT journal_offset, journal_sequence "
        "FROM materialization_checkpoint "
        "WHERE checkpoint_key = 'materialization';",
        &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships expunge checkpoint lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_row_count (&result) == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships expunge materialization checkpoint is missing");
        return FALSE;
    }

    *out_offset = (guint64)duckdb_value_uint64 (&result, 0, 0);
    *out_sequence = (guint64)duckdb_value_uint64 (&result, 1, 0);
    if (*out_sequence == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships expunge materialization checkpoint is invalid");
        return FALSE;
    }

    return TRUE;
}

static gboolean
query_mailbox_uidvalidity (MembershipsExpungeJournal *self,
    const WyreboxDaemonMembershipsExpungeRequest *request,
    duckdb_result *out_result, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
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
            "memberships expunge prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->mailbox_id) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "memberships expunge bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, out_result) == DuckDBSuccess)
        return TRUE;

    detail = duckdb_result_error (out_result);
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "memberships expunge mailbox lookup failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
}

static gboolean
lookup_derived_view (MembershipsExpungeJournal *self,
    const WyreboxDaemonMembershipsExpungeRequest *request,
    gboolean *out_is_derived_view, GError **error)
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
            "memberships expunge derived view prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->mailbox_id) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "memberships expunge bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships expunge derived view lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    *out_is_derived_view = duckdb_row_count (&result) > 0;
    return TRUE;
}

static gboolean
check_target (MembershipsExpungeJournal *self,
    const WyreboxDaemonMembershipsExpungeRequest *request, GError **error)
{
    g_auto (duckdb_result) mailbox = { 0 };

    if (!query_mailbox_uidvalidity (self, request, &mailbox, error))
        return FALSE;

    if (duckdb_row_count (&mailbox) == 0) {
        gboolean is_derived_view = FALSE;

        if (!lookup_derived_view (self, request, &is_derived_view, error))
            return FALSE;

        if (is_derived_view) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_PERMISSION_DENIED,
                "expunge is not permitted for virtual mailbox %s",
                request->mailbox_id);
            return FALSE;
        }

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

    return TRUE;
}

static gboolean
lookup_uid_visible (MembershipsExpungeJournal *self,
    const WyreboxDaemonMembershipsExpungeRequest *request, guint64 uid,
    gboolean *out_visible, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    const char *detail = NULL;

    if (duckdb_prepare (self->connection,
        "SELECT 1 FROM mailbox_memberships "
        "WHERE account_id = ? AND mailbox_id = ? AND uid = ? "
        "AND is_visible = TRUE;", &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships expunge membership prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->mailbox_id) != DuckDBSuccess ||
        duckdb_bind_uint64 (statement, 3, uid) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "memberships expunge bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships expunge membership lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    *out_visible = duckdb_row_count (&result) > 0;
    return TRUE;
}

static guint64 *
filter_visible_uids (MembershipsExpungeJournal *self,
    const WyreboxDaemonMembershipsExpungeRequest *request, gsize *out_n_uids,
    GError **error)
{
    g_autofree guint64 *filtered = NULL;
    gsize count = 0;

    g_return_val_if_fail (out_n_uids != NULL, NULL);

    *out_n_uids = 0;
    filtered = g_new0 (guint64, request->n_uids);

    for (gsize i = 0; i < request->n_uids; i++) {
        gboolean visible = FALSE;

        if (!lookup_uid_visible (self, request, request->uids[i], &visible,
            error))
            return NULL;

        if (!visible)
            continue;

        filtered[count++] = request->uids[i];
    }

    *out_n_uids = count;
    return g_steal_pointer (&filtered);
}

static gboolean
append_memberships_expunged (MembershipsExpungeJournal *self,
    const WyreboxDaemonMembershipsExpungeRequest *request,
    const guint64 *uids, gsize n_uids, guint64 *out_offset,
    guint64 *out_sequence, GError **error)
{
    WyreboxMembershipsExpungedPayload payload = {
        .account_id = request->account_identity,
        .mailbox_id = request->mailbox_id,
        .uidvalidity = request->uid_validity,
        .uids = (guint64 *)uids,
        .n_uids = n_uids,
    };
    g_autoptr (GBytes) bytes = NULL;

    bytes = wyrebox_memberships_expunged_payload_encode (&payload, error);
    if (bytes == NULL)
        return FALSE;

    return wyrebox_journal_writer_append (self->journal_writer,
               WYREBOX_JOURNAL_EVENT_MEMBERSHIPS_EXPUNGED, bytes, out_offset,
               out_sequence, error);
}

static void
fill_noop_receipt (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMembershipsExpungeRequest *request,
    guint64 checkpoint_offset, guint64 checkpoint_sequence,
    WyreboxDaemonSuccessReceipt *out_receipt)
{
    wyrebox_daemon_success_receipt_clear (out_receipt);
    out_receipt->request_id = g_strdup (identity->request_id);
    out_receipt->durable_marker = g_strdup ("no-op");
    out_receipt->journal_offset = checkpoint_offset;
    out_receipt->journal_sequence = checkpoint_sequence;
    out_receipt->summary = g_strdup_printf ("memberships_expunge no-op "
            "mailbox_id=%s uid_count=%" G_GSIZE_FORMAT, request->mailbox_id,
            request->n_uids);
}

static void
fill_journaled_receipt (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMembershipsExpungeRequest *request, gsize n_uids,
    guint64 journal_offset, guint64 journal_sequence,
    WyreboxDaemonSuccessReceipt *out_receipt)
{
    wyrebox_daemon_success_receipt_clear (out_receipt);
    out_receipt->request_id = g_strdup (identity->request_id);
    out_receipt->durable_marker = g_strdup_printf ("journal:%"
            G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT, journal_offset,
            journal_sequence);
    out_receipt->journal_offset = journal_offset;
    out_receipt->journal_sequence = journal_sequence;
    out_receipt->summary = g_strdup_printf ("memberships_expunge "
            "mailbox_id=%s uid_count=%" G_GSIZE_FORMAT, request->mailbox_id,
            n_uids);
}

static gboolean
expunge_memberships (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMembershipsExpungeRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt, gpointer user_data,
    GError **error)
{
    MembershipsExpungeJournal *self = user_data;
    g_autofree guint64 *visible_uids = NULL;
    gsize n_visible_uids = 0;
    guint64 journal_offset = 0;
    guint64 journal_sequence = 0;
    guint64 checkpoint_offset = 0;
    guint64 checkpoint_sequence = 0;

    {
        g_autoptr (GMutexLocker) locker =
            g_mutex_locker_new (&self->connection_mutex);

        if (!check_target (self, request, error))
            return FALSE;

        visible_uids = filter_visible_uids (self, request, &n_visible_uids,
                error);
        if (visible_uids == NULL)
            return FALSE;

        if (n_visible_uids == 0) {
            if (!load_materialization_checkpoint (self, &checkpoint_offset,
                &checkpoint_sequence, error))
                return FALSE;

            fill_noop_receipt (identity, request, checkpoint_offset,
                checkpoint_sequence, out_receipt);
            return TRUE;
        }
    }

    if (!append_memberships_expunged (self, request, visible_uids,
        n_visible_uids, &journal_offset, &journal_sequence, error))
        return FALSE;

    wyrebox_daemon_delivery_materialization_catch_up_or_schedule_retry
        (self->materialization);

    fill_journaled_receipt (identity, request, n_visible_uids, journal_offset,
        journal_sequence, out_receipt);
    return TRUE;
}

WyreboxDaemonMembershipsExpungeService *
wyrebox_daemon_memberships_expunge_service_new_journaled (const char
    *catalog_path, WyreboxJournalWriter *journal_writer,
    WyreboxDaemonDeliveryMaterialization *materialization, GError **error)
{
    g_autoptr (MembershipsExpungeJournal) self = NULL;

    g_return_val_if_fail (catalog_path != NULL, NULL);
    g_return_val_if_fail (WYREBOX_IS_JOURNAL_WRITER (journal_writer), NULL);
    g_return_val_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION
            (materialization), NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    self = g_new0 (MembershipsExpungeJournal, 1);
    g_mutex_init (&self->connection_mutex);
    self->journal_writer = g_object_ref (journal_writer);
    self->materialization = g_object_ref (materialization);

    if (!wyrebox_duckdb_open_shared (catalog_path, &self->database, error))
        return NULL;

    if (duckdb_connect (self->database, &self->connection) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships expunge connect failed for '%s'", catalog_path);
        return NULL;
    }

    return wyrebox_daemon_memberships_expunge_service_new (expunge_memberships,
               g_steal_pointer (&self), memberships_expunge_journal_free);
}
