#include "wyrebox-daemon-message-search-duckdb.h"

#include "wyrebox-duckdb-shared.h"

#include <duckdb.h>
#include <gio/gio.h>

#define ASCII_UPPER "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
#define ASCII_LOWER "abcdefghijklmnopqrstuvwxyz"

typedef struct
{
    GMutex connection_mutex;
    duckdb_database database;
    duckdb_connection connection;
} MessageSearchDuckDB;

typedef struct
{
    const char *namespace_kind;
    const char *namespace_check_sql;
    const char *membership_sql;
} MessageSearchNamespace;

static const MessageSearchNamespace mailbox_namespace = {
    "mailbox",
    "SELECT us.uidvalidity FROM mailbox_uid_state us "
    "JOIN mailboxes mb ON mb.account_id = us.account_id "
    "AND mb.mailbox_id = us.namespace_id "
    "WHERE us.account_id = ? AND us.namespace_kind = 'mailbox' "
    "AND us.namespace_id = ? "
    "AND mb.is_visible = TRUE AND mb.is_selectable = TRUE;",
    "SELECT m.uid FROM mailbox_memberships m "
    "LEFT JOIN message_headers mh ON mh.message_id = m.message_id "
    "WHERE m.account_id = ? AND m.mailbox_id = ? AND m.is_visible = TRUE",
};

static const MessageSearchNamespace derived_view_namespace = {
    "derived_view",
    "SELECT us.uidvalidity FROM mailbox_uid_state us "
    "JOIN derived_views dv ON dv.account_id = us.account_id "
    "AND dv.view_id = us.namespace_id "
    "WHERE us.account_id = ? AND us.namespace_kind = 'derived_view' "
    "AND us.namespace_id = ? "
    "AND dv.is_visible = TRUE AND dv.is_selectable = TRUE;",
    "SELECT m.uid FROM derived_view_memberships m "
    "LEFT JOIN message_headers mh ON mh.message_id = m.message_id "
    "WHERE m.account_id = ? AND m.view_id = ? AND m.is_visible = TRUE",
};

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
message_search_duckdb_free (gpointer data)
{
    MessageSearchDuckDB *self = data;

    if (self->connection != NULL)
        duckdb_disconnect (&self->connection);
    if (self->database != NULL)
        duckdb_close (&self->database);
    g_mutex_clear (&self->connection_mutex);
    g_free (self);
}

/* *INDENT-OFF* */
G_DEFINE_AUTOPTR_CLEANUP_FUNC (MessageSearchDuckDB, message_search_duckdb_free)
/* *INDENT-ON* */

static gboolean
prepare (MessageSearchDuckDB *self, const char *sql,
    duckdb_prepared_statement *out_statement, GError **error)
{
    if (duckdb_prepare (self->connection, sql, out_statement) == DuckDBSuccess)
        return TRUE;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "message SEARCH prepare failed: %s",
        *out_statement != NULL ?
        duckdb_prepare_error (*out_statement) : "unknown DuckDB error");
    return FALSE;
}

static gboolean
execute (duckdb_prepared_statement statement, duckdb_result *out_result,
    GError **error)
{
    const char *detail = NULL;

    if (duckdb_execute_prepared (statement, out_result) == DuckDBSuccess)
        return TRUE;

    detail = duckdb_result_error (out_result);
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "message SEARCH query failed: %s",
        detail != NULL ? detail : "unknown DuckDB error");
    return FALSE;
}

static gboolean
bind_namespace (duckdb_prepared_statement statement,
    const WyreboxDaemonMessageSearchRequest *request, GError **error)
{
    if (duckdb_bind_varchar (statement, 1,
        request->account_identity) == DuckDBSuccess &&
        duckdb_bind_varchar (statement, 2,
        request->mailbox_id) == DuckDBSuccess)
        return TRUE;

    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_FAILED, "message SEARCH bind failed");
    return FALSE;
}

static gboolean
check_namespace (MessageSearchDuckDB *self,
    const MessageSearchNamespace *namespace,
    const WyreboxDaemonMessageSearchRequest *request, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };

    if (!prepare (self, namespace->namespace_check_sql, &statement, error) ||
        !bind_namespace (statement, request, error) ||
        !execute (statement, &result, error))
        return FALSE;

    if (duckdb_row_count (&result) == 0) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_FOUND,
            "selectable %s %s was not found for account %s",
            namespace->namespace_kind, request->mailbox_id,
            request->account_identity);
        return FALSE;
    }

    if ((guint64)duckdb_value_uint64 (&result, 0, 0) != request->uid_validity) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_EXISTS,
            "UIDVALIDITY mismatch for %s %s", namespace->namespace_kind,
            request->mailbox_id);
        return FALSE;
    }

    return TRUE;
}

static void
append_ascii_folded_contains (GString *sql, const char *column)
{
    g_string_append_printf (sql, " AND %s IS NOT NULL AND translate(%s, '"
        ASCII_UPPER "', '" ASCII_LOWER "') LIKE ? ESCAPE '\\'", column,
        column);
}

/* Lowercases ASCII letters and escapes LIKE controls in @text. */
static char *
make_ascii_folded_contains_pattern (const char *text)
{
    g_autofree char *folded = g_ascii_strdown (text, -1);
    GString *pattern = g_string_new ("%");

    for (const char *cursor = folded; *cursor != '\0'; cursor++) {
        if (*cursor == '%' || *cursor == '_' || *cursor == '\\')
            g_string_append_c (pattern, '\\');
        g_string_append_c (pattern, *cursor);
    }
    g_string_append_c (pattern, '%');

    return g_string_free (pattern, FALSE);
}

static char *
build_search_sql (const MessageSearchNamespace *namespace,
    const WyreboxDaemonMessageSearchRequest *request)
{
    GString *sql = g_string_new (namespace->membership_sql);

    for (guint i = 0; i < request->n_criteria; i++) {
        switch (request->criteria[i].kind) {
        case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SUBJECT_CONTAINS:
            append_ascii_folded_contains (sql, "mh.subject");
            break;
        case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_FROM_CONTAINS:
            append_ascii_folded_contains (sql, "mh.from_addr");
            break;
        case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENDER_DOMAIN:
            g_string_append (sql, " AND mh.sender_domain = ?");
            break;
        case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENT_SINCE:
            g_string_append (sql, " AND mh.date_unix_us >= ?");
            break;
        case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENT_BEFORE:
            g_string_append (sql, " AND mh.date_unix_us < ?");
            break;
        default:
            g_assert_not_reached ();
        }
    }
    g_string_append (sql, " ORDER BY m.uid;");

    return g_string_free (sql, FALSE);
}

static gboolean
bind_criterion (duckdb_prepared_statement statement, idx_t index,
    const WyreboxDaemonMessageSearchCriterion *criterion, GError **error)
{
    g_autofree char *value = NULL;
    duckdb_state state = DuckDBError;

    switch (criterion->kind) {
    case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SUBJECT_CONTAINS:
    case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_FROM_CONTAINS:
        value = make_ascii_folded_contains_pattern (criterion->text);
        state = duckdb_bind_varchar (statement, index, value);
        break;
    case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENDER_DOMAIN:
        value = g_ascii_strdown (criterion->text, -1);
        state = duckdb_bind_varchar (statement, index, value);
        break;
    case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENT_SINCE:
    case WYREBOX_DAEMON_MESSAGE_SEARCH_CRITERION_SENT_BEFORE:
        state = duckdb_bind_int64 (statement, index, criterion->unix_us);
        break;
    default:
        g_assert_not_reached ();
    }

    if (state == DuckDBSuccess)
        return TRUE;

    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_FAILED, "message SEARCH criterion bind failed");
    return FALSE;
}

static GBytes *
query_matching_uids (MessageSearchDuckDB *self,
    const MessageSearchNamespace *namespace,
    const WyreboxDaemonMessageSearchRequest *request, GError **error)
{
    g_autofree char *sql = build_search_sql (namespace, request);
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    g_autoptr (GString) uids = NULL;

    if (!prepare (self, sql, &statement, error) ||
        !bind_namespace (statement, request, error))
        return NULL;

    for (guint i = 0; i < request->n_criteria; i++) {
        if (!bind_criterion (statement, i + 3, &request->criteria[i], error))
            return NULL;
    }

    if (!execute (statement, &result, error))
        return NULL;

    uids = g_string_new (NULL);
    for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
        g_string_append_printf (uids, "%" G_GUINT64_FORMAT "\n",
            (guint64)duckdb_value_uint64 (&result, 0, row));
    }

    return g_string_free_to_bytes (g_steal_pointer (&uids));
}

static gboolean
search_messages (const WyreboxDaemonRequestIdentity *identity,
    const WyreboxDaemonMessageSearchRequest *request,
    WyreboxDaemonStreamChunkFrame *out_chunk, gpointer user_data,
    GError **error)
{
    MessageSearchDuckDB *self = user_data;
    const MessageSearchNamespace *namespace =
        request->namespace_kind == WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_VIRTUAL ?
        &derived_view_namespace : &mailbox_namespace;
    g_autoptr (GBytes) uids = NULL;

    {
        g_autoptr (GMutexLocker) locker =
            g_mutex_locker_new (&self->connection_mutex);

        if (!check_namespace (self, namespace, request, error))
            return FALSE;

        uids = query_matching_uids (self, namespace, request, error);
        if (uids == NULL)
            return FALSE;
    }

    return wyrebox_daemon_stream_chunk_frame_init (out_chunk,
               identity->request_id, NULL,
               WYREBOX_DAEMON_MESSAGE_SEARCH_DUCKDB_QUERY_ID,
               identity->correlation_id, 0, uids, TRUE, error);
}

WyreboxDaemonMessageSearchService *
wyrebox_daemon_message_search_service_new_duckdb (const char *catalog_path,
    GError **error)
{
    g_autoptr (MessageSearchDuckDB) self = NULL;

    g_return_val_if_fail (catalog_path != NULL, NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    self = g_new0 (MessageSearchDuckDB, 1);
    g_mutex_init (&self->connection_mutex);

    if (!wyrebox_duckdb_open_shared (catalog_path, &self->database, error))
        return NULL;

    if (duckdb_connect (self->database, &self->connection) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "message SEARCH connect failed for '%s'", catalog_path);
        return NULL;
    }

    return wyrebox_daemon_message_search_service_new (search_messages,
               g_steal_pointer (&self), message_search_duckdb_free);
}
