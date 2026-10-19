#include "wyrebox-daemon-memberships-copy-move-journal.h"

#include "wyrebox-duckdb-shared.h"
#include "wyrebox-memberships-copied-payload.h"

#include <duckdb.h>
#include <gio/gio.h>

typedef struct
{
    GMutex connection_mutex;
    duckdb_database database;
    duckdb_connection connection;
    WyreboxJournalWriter *journal_writer;
    WyreboxDaemonDeliveryMaterialization *materialization;
} MembershipsCopyMoveJournal;

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
memberships_copy_move_journal_free (gpointer data)
{
    MembershipsCopyMoveJournal *self = data;

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
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MembershipsCopyMoveJournal,
    memberships_copy_move_journal_free)
/* *INDENT-ON* */

static gboolean
lookup_derived_view (MembershipsCopyMoveJournal *self,
    const char *account_id, const char *view_id,
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
            "memberships copy/move derived view prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1, account_id) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2, view_id) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "memberships copy/move bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move derived view lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    *out_is_derived_view = duckdb_row_count (&result) > 0;
    return TRUE;
}

static gboolean
check_ordinary_mailbox (MembershipsCopyMoveJournal *self,
    const char *account_id, const char *mailbox_id, guint64 uid_validity,
    const char *what, GError **error)
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
            "memberships copy/move %s prepare failed: %s",
            what,
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1, account_id) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2, mailbox_id) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "memberships copy/move bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move %s lookup failed: %s",
            what, detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_row_count (&result) == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "selectable mailbox %s was not found for account %s",
            mailbox_id, account_id);
        return FALSE;
    }

    if ((guint64)duckdb_value_uint64 (&result, 0,
        0) != uid_validity) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_EXISTS,
            "UIDVALIDITY mismatch for mailbox %s", mailbox_id);
        return FALSE;
    }

    return TRUE;
}

static gboolean
check_dest_mailbox (MembershipsCopyMoveJournal *self,
    const WyreboxDaemonMembershipsCopyMoveRequest *request, GError **error)
{
    gboolean is_derived_view = FALSE;

    if (!lookup_derived_view (self, request->account_identity,
        request->dest_mailbox_id, &is_derived_view, error))
        return FALSE;

    if (is_derived_view) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_PERMISSION_DENIED,
            "copy/move destination is not permitted for virtual mailbox %s",
            request->dest_mailbox_id);
        return FALSE;
    }

    return check_ordinary_mailbox (self, request->account_identity,
               request->dest_mailbox_id, request->dest_uid_validity,
               "destination", error);
}

static gboolean
check_source_mailbox (MembershipsCopyMoveJournal *self, gboolean is_move,
    const WyreboxDaemonMembershipsCopyMoveRequest *request, GError **error)
{
    if (is_move &&
        request->source_namespace_kind ==
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_PERMISSION_DENIED,
            "move is not permitted for virtual mailbox %s",
            request->source_mailbox_id);
        return FALSE;
    }

    if (request->source_namespace_kind ==
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY) {
        return check_ordinary_mailbox (self, request->account_identity,
                   request->source_mailbox_id, request->source_uid_validity,
                   "source", error);
    }

    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    const char *detail = NULL;
    gboolean is_derived_view = FALSE;

    if (!lookup_derived_view (self, request->account_identity,
        request->source_mailbox_id, &is_derived_view, error))
        return FALSE;

    if (!is_derived_view) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "selectable virtual mailbox %s was not found for account %s",
            request->source_mailbox_id, request->account_identity);
        return FALSE;
    }

    if (duckdb_prepare (self->connection,
        "SELECT us.uidvalidity FROM mailbox_uid_state us "
        "JOIN derived_views dv ON dv.account_id = us.account_id "
        "AND dv.view_id = us.namespace_id "
        "WHERE us.account_id = ? AND us.namespace_kind = 'derived_view' "
        "AND us.namespace_id = ? "
        "AND dv.is_visible = TRUE AND dv.is_selectable = TRUE;",
        &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move virtual source prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->source_mailbox_id) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "memberships copy/move bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move virtual source lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_row_count (&result) == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "selectable virtual mailbox %s was not found for account %s",
            request->source_mailbox_id, request->account_identity);
        return FALSE;
    }

    if ((guint64)duckdb_value_uint64 (&result, 0,
        0) != request->source_uid_validity) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_EXISTS,
            "UIDVALIDITY mismatch for virtual mailbox %s",
            request->source_mailbox_id);
        return FALSE;
    }

    return TRUE;
}

static gboolean
lookup_ordinary_source (MembershipsCopyMoveJournal *self,
    const WyreboxDaemonMembershipsCopyMoveRequest *request, guint64 src_uid,
    char **out_message_id, char **out_membership_id, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    const char *detail = NULL;
    char *message_id = NULL;
    char *membership_id = NULL;

    if (duckdb_prepare (self->connection,
        "SELECT mm.message_id, mm.membership_id FROM mailbox_memberships mm "
        "JOIN mailbox_uid_state mus ON mus.account_id = mm.account_id "
        "AND mus.namespace_kind = 'mailbox' "
        "AND mus.namespace_id = mm.mailbox_id "
        "WHERE mm.account_id = ? AND mm.mailbox_id = ? AND mm.uid = ? "
        "AND mus.uidvalidity = ? AND mm.is_visible = TRUE;",
        &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move source prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->source_mailbox_id) != DuckDBSuccess ||
        duckdb_bind_uint64 (statement, 3, src_uid) != DuckDBSuccess ||
        duckdb_bind_uint64 (statement, 4,
        request->source_uid_validity) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "memberships copy/move bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move source lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_row_count (&result) == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "UID %" G_GUINT64_FORMAT " was not found in mailbox %s",
            src_uid, request->source_mailbox_id);
        return FALSE;
    }

    message_id = duckdb_value_varchar (&result, 0, 0);
    membership_id = duckdb_value_varchar (&result, 1, 0);
    if (message_id == NULL || membership_id == NULL) {
        if (message_id != NULL)
            duckdb_free (message_id);
        if (membership_id != NULL)
            duckdb_free (membership_id);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move source lookup returned malformed row");
        return FALSE;
    }

    *out_message_id = g_strdup (message_id);
    *out_membership_id = g_strdup (membership_id);
    duckdb_free (message_id);
    duckdb_free (membership_id);
    return TRUE;
}

static gboolean
lookup_virtual_source (MembershipsCopyMoveJournal *self,
    const WyreboxDaemonMembershipsCopyMoveRequest *request, guint64 src_uid,
    char **out_message_id, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    const char *detail = NULL;
    char *message_id = NULL;

    if (duckdb_prepare (self->connection,
        "SELECT dvm.message_id FROM derived_view_memberships dvm "
        "JOIN derived_views dv ON dv.account_id = dvm.account_id "
        "AND dv.view_id = dvm.view_id "
        "JOIN mailbox_uid_state mus ON mus.account_id = dvm.account_id "
        "AND mus.namespace_kind = 'derived_view' "
        "AND mus.namespace_id = dvm.view_id "
        "WHERE dvm.account_id = ? AND dvm.view_id = ? AND dvm.uid = ? "
        "AND mus.uidvalidity = ? AND dvm.is_visible = TRUE "
        "AND dv.is_visible = TRUE AND dv.is_selectable = TRUE;",
        &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move virtual source prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->source_mailbox_id) != DuckDBSuccess ||
        duckdb_bind_uint64 (statement, 3, src_uid) != DuckDBSuccess ||
        duckdb_bind_uint64 (statement, 4,
        request->source_uid_validity) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "memberships copy/move bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move virtual source lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_row_count (&result) == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "UID %" G_GUINT64_FORMAT " was not found in virtual mailbox %s",
            src_uid, request->source_mailbox_id);
        return FALSE;
    }

    message_id = duckdb_value_varchar (&result, 0, 0);
    if (message_id == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move virtual source lookup returned malformed "
            "row");
        return FALSE;
    }

    *out_message_id = g_strdup (message_id);
    duckdb_free (message_id);
    return TRUE;
}

static gboolean
lookup_min_internal_date (MembershipsCopyMoveJournal *self,
    const char *account_id, const char *message_id,
    guint64 *out_internal_date_unix_us, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    const char *detail = NULL;

    if (duckdb_prepare (self->connection,
        "SELECT MIN(internal_date_unix_us) FROM mailbox_memberships "
        "WHERE account_id = ? AND message_id = ?;",
        &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move internal date prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1, account_id) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2, message_id) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "memberships copy/move bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move internal date lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_row_count (&result) == 0 ||
        duckdb_value_is_null (&result, 0, 0)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "message %s has no ordinary memberships for internal date",
            message_id);
        return FALSE;
    }

    *out_internal_date_unix_us = (guint64)duckdb_value_uint64 (&result, 0, 0);
    return TRUE;
}

static GStrv
load_flag_names (MembershipsCopyMoveJournal *self, const char *table,
    const char *column, const char *membership_id, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    const char *detail = NULL;
    g_autoptr (GPtrArray) names = g_ptr_array_new ();
    gsize row_count = 0;

    g_autofree char *query = g_strdup_printf ("SELECT %s FROM %s "
            "WHERE membership_id = ? ORDER BY %s ASC;",
            column, table, column);

    if (duckdb_prepare (self->connection, query, &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move flag prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return NULL;
    }

    if (duckdb_bind_varchar (statement, 1, membership_id) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "memberships copy/move bind failed");
        return NULL;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move flag lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return NULL;
    }

    row_count = (gsize)duckdb_row_count (&result);
    for (gsize i = 0; i < row_count; i++) {
        char *name = duckdb_value_varchar (&result, 0, (idx_t)i);

        if (name == NULL)
            continue;
        g_ptr_array_add (names, g_strdup (name));
        duckdb_free (name);
    }

    g_ptr_array_add (names, NULL);
    return (GStrv)g_ptr_array_free (g_steal_pointer (&names), FALSE);
}

static gboolean
reserve_destination_uids (MembershipsCopyMoveJournal *self,
    const WyreboxDaemonMembershipsCopyMoveRequest *request, gsize n_items,
    guint64 *out_first_uid, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    const char *detail = NULL;
    guint64 uidnext = 0;
    guint64 reserved_end = 0;

    if (duckdb_prepare (self->connection,
        "SELECT uidnext FROM mailbox_uid_state "
        "WHERE account_id = ? AND namespace_kind = 'mailbox' "
        "AND namespace_id = ? AND uidvalidity = ?;",
        &statement) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move uidnext prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_varchar (statement, 1,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->dest_mailbox_id) != DuckDBSuccess ||
        duckdb_bind_uint64 (statement, 3,
        request->dest_uid_validity) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "memberships copy/move bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move uidnext lookup failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_row_count (&result) != 1) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "destination mailbox %s uid state was not found",
            request->dest_mailbox_id);
        return FALSE;
    }

    uidnext = (guint64)duckdb_value_uint64 (&result, 0, 0);
    if (uidnext == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "destination mailbox %s uidnext is invalid",
            request->dest_mailbox_id);
        return FALSE;
    }

    if (n_items > G_MAXUINT64 - uidnext) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_ARGUMENT,
            "memberships copy/move destination uid reservation overflows");
        return FALSE;
    }

    reserved_end = uidnext + n_items;
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
            "memberships copy/move uidnext update prepare failed: %s",
            statement != NULL ?
            duckdb_prepare_error (statement) : "unknown DuckDB error");
        return FALSE;
    }

    if (duckdb_bind_uint64 (statement, 1, reserved_end) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 2,
        request->account_identity) != DuckDBSuccess ||
        duckdb_bind_varchar (statement, 3,
        request->dest_mailbox_id) != DuckDBSuccess ||
        duckdb_bind_uint64 (statement, 4,
        request->dest_uid_validity) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR, G_IO_ERROR_FAILED, "memberships copy/move bind failed");
        return FALSE;
    }

    if (duckdb_execute_prepared (statement, &result) != DuckDBSuccess) {
        detail = duckdb_result_error (&result);
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move uidnext update failed: %s",
            detail != NULL ? detail : "unknown DuckDB error");
        return FALSE;
    }

    *out_first_uid = uidnext;
    return TRUE;
}

typedef struct
{
    char *message_id;
    GStrv system_flags;
    GStrv user_keywords;
    guint64 internal_date_unix_us;
} CopyMoveResolvedSource;

static void
copy_move_resolved_source_clear (CopyMoveResolvedSource *resolved)
{
    if (resolved == NULL)
        return;

    g_clear_pointer (&resolved->message_id, g_free);
    g_clear_pointer (&resolved->system_flags, g_strfreev);
    g_clear_pointer (&resolved->user_keywords, g_strfreev);
    resolved->internal_date_unix_us = 0;
}

static void
clear_resolved_sources (CopyMoveResolvedSource *resolved, gsize n)
{
    if (resolved == NULL)
        return;

    for (gsize i = 0; i < n; i++)
        copy_move_resolved_source_clear (&resolved[i]);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (CopyMoveResolvedSource,
    copy_move_resolved_source_clear)
/* *INDENT-ON* */

static gboolean
resolve_copy_move_source (MembershipsCopyMoveJournal *self,
    const WyreboxDaemonMembershipsCopyMoveRequest *request, guint64 src_uid,
    CopyMoveResolvedSource *out_resolved, GError **error)
{
    g_autofree char *message_id = NULL;
    g_autofree char *membership_id = NULL;

    if (request->source_namespace_kind ==
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY) {
        if (!lookup_ordinary_source (self, request, src_uid, &message_id,
            &membership_id, error))
            return FALSE;

        out_resolved->system_flags = load_flag_names (self, "message_flags",
                "flag_name", membership_id, error);
        if (out_resolved->system_flags == NULL)
            return FALSE;

        out_resolved->user_keywords = load_flag_names (self,
                "message_keywords", "keyword_name", membership_id, error);
        if (out_resolved->user_keywords == NULL)
            return FALSE;
    } else {
        if (!lookup_virtual_source (self, request, src_uid, &message_id, error))
            return FALSE;

        out_resolved->system_flags = g_new0 (char *, 1);
        out_resolved->user_keywords = g_new0 (char *, 1);
    }

    if (!lookup_min_internal_date (self, request->account_identity,
        message_id, &out_resolved->internal_date_unix_us, error))
        return FALSE;

    out_resolved->message_id = g_steal_pointer (&message_id);
    return TRUE;
}

static gboolean
build_payload (MembershipsCopyMoveJournal *self,
    const WyreboxDaemonMembershipsCopyMoveRequest *request,
    WyreboxMembershipsCopiedPayload *out_payload, GError **error)
{
    g_autofree WyreboxMembershipsCopiedItem *items = NULL;
    g_autofree CopyMoveResolvedSource *resolved = NULL;
    guint64 first_dst_uid = 0;

    items = g_new0 (WyreboxMembershipsCopiedItem, request->n_source_uids);
    resolved = g_new0 (CopyMoveResolvedSource, request->n_source_uids);

    for (gsize i = 0; i < request->n_source_uids; i++) {
        if (!resolve_copy_move_source (self, request, request->source_uids[i],
            &resolved[i], error)) {
            clear_resolved_sources (resolved, request->n_source_uids);
            return FALSE;
        }
    }

    if (!reserve_destination_uids (self, request, request->n_source_uids,
        &first_dst_uid, error)) {
        clear_resolved_sources (resolved, request->n_source_uids);
        return FALSE;
    }

    for (gsize i = 0; i < request->n_source_uids; i++) {
        WyreboxMembershipsCopiedItem *item = &items[i];

        item->src_uid = request->source_uids[i];
        item->dst_uid = first_dst_uid + i;
        item->message_id = g_strdup (resolved[i].message_id);
        item->internal_date_unix_us = resolved[i].internal_date_unix_us;
        item->system_flags = g_strdupv (resolved[i].system_flags);
        item->user_keywords = g_strdupv (resolved[i].user_keywords);
    }

    out_payload->account_id = g_strdup (request->account_identity);
    out_payload->source_mailbox_id = g_strdup (request->source_mailbox_id);
    out_payload->dest_mailbox_id = g_strdup (request->dest_mailbox_id);
    out_payload->source_uidvalidity = request->source_uid_validity;
    out_payload->dest_uidvalidity = request->dest_uid_validity;
    out_payload->items = g_steal_pointer (&items);
    out_payload->n_items = request->n_source_uids;
    clear_resolved_sources (resolved, request->n_source_uids);

    return TRUE;
}

static gboolean
append_memberships_copy_move (MembershipsCopyMoveJournal *self,
    gboolean is_move, const WyreboxMembershipsCopiedPayload *payload,
    guint64 *out_offset, guint64 *out_sequence, GError **error)
{
    g_autoptr (GBytes) bytes = NULL;
    WyreboxJournalEventType event_type = is_move ?
        WYREBOX_JOURNAL_EVENT_MEMBERSHIPS_MOVED :
        WYREBOX_JOURNAL_EVENT_MEMBERSHIPS_COPIED;

    bytes = wyrebox_memberships_copied_payload_encode (payload, error);
    if (bytes == NULL)
        return FALSE;

    return wyrebox_journal_writer_append (self->journal_writer, event_type,
               bytes, out_offset, out_sequence, error);
}

static char *
format_summary (gboolean is_move,
    const WyreboxDaemonMembershipsCopyMoveRequest *request,
    const WyreboxMembershipsCopiedPayload *payload)
{
    GString *summary = g_string_new (NULL);

    g_string_append (summary, is_move ? "memberships_moved" :
        "memberships_copied");
    g_string_append_printf (summary, " dest_mailbox_id=%s dest_uidvalidity=%"
        G_GUINT64_FORMAT " pairs=",
        request->dest_mailbox_id, request->dest_uid_validity);

    for (gsize i = 0; i < payload->n_items; i++) {
        if (i > 0)
            g_string_append_c (summary, ',');
        g_string_append_printf (summary, "%" G_GUINT64_FORMAT ":%"
            G_GUINT64_FORMAT,
            payload->items[i].src_uid, payload->items[i].dst_uid);
    }

    return g_string_free (summary, FALSE);
}

static gboolean
copy_move_memberships (const WyreboxDaemonRequestIdentity *identity,
    gboolean is_move, const WyreboxDaemonMembershipsCopyMoveRequest *request,
    WyreboxDaemonSuccessReceipt *out_receipt, gpointer user_data,
    GError **error)
{
    MembershipsCopyMoveJournal *self = user_data;
    g_auto (WyreboxMembershipsCopiedPayload) payload = { 0 };
    guint64 journal_offset = 0;
    guint64 journal_sequence = 0;

    if (!wyrebox_daemon_delivery_materialization_catch_up (
            self->materialization,
            error))
        return FALSE;

    {
        g_autoptr (GMutexLocker) locker =
            g_mutex_locker_new (&self->connection_mutex);

        if (!check_dest_mailbox (self, request, error))
            return FALSE;

        if (!check_source_mailbox (self, is_move, request, error))
            return FALSE;

        if (!build_payload (self, request, &payload, error))
            return FALSE;
    }

    if (!append_memberships_copy_move (self, is_move, &payload,
        &journal_offset, &journal_sequence, error))
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
    out_receipt->summary = format_summary (is_move, request, &payload);
    return TRUE;
}

WyreboxDaemonMembershipsCopyMoveService *
wyrebox_daemon_memberships_copy_move_service_new_journaled (const char
    *catalog_path, WyreboxJournalWriter *journal_writer,
    WyreboxDaemonDeliveryMaterialization *materialization, GError **error)
{
    g_autoptr (MembershipsCopyMoveJournal) self = NULL;

    g_return_val_if_fail (catalog_path != NULL, NULL);
    g_return_val_if_fail (WYREBOX_IS_JOURNAL_WRITER (journal_writer), NULL);
    g_return_val_if_fail (WYREBOX_IS_DAEMON_DELIVERY_MATERIALIZATION
            (materialization), NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    self = g_new0 (MembershipsCopyMoveJournal, 1);
    g_mutex_init (&self->connection_mutex);
    self->journal_writer = g_object_ref (journal_writer);
    self->materialization = g_object_ref (materialization);

    if (!wyrebox_duckdb_open_shared (catalog_path, &self->database, error))
        return NULL;

    if (duckdb_connect (self->database, &self->connection) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "memberships copy/move connect failed for '%s'", catalog_path);
        return NULL;
    }

    return wyrebox_daemon_memberships_copy_move_service_new (
        copy_move_memberships,
        g_steal_pointer (&self), memberships_copy_move_journal_free);
}
