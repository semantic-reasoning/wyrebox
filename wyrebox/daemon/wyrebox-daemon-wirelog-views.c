#include "wyrebox-daemon-wirelog-views.h"

#include "wyrebox-derived-view-materializer.h"
#include "wyrebox-duckdb-shared.h"
#include "wyrebox-fact-record.h"
#include "wyrebox-wirelog-derived-membership.h"
#include "wyrebox-wirelog-program.h"
#include "wyrebox-wirelog-rule-version.h"

#include <gio/gio.h>
#include <string.h>

typedef struct
{
    char *view_id;
    char *imap_name;
    char *definition_ref;
    WyreboxDaemonViewScope scope;
} WirelogView;

typedef struct
{
    char *message_id;
    char *link_key;
} ThreadLink;

struct _WyreboxDaemonWirelogViews
{
    GObject parent_instance;

    char *rules_path;
    char *rules_source;
    char *rule_version_hash;
    GHashTable *declared_predicates;
    GPtrArray *views;
    GPtrArray *known_symbols;
    WyreboxJournalWriter *journal_writer;
    WyreboxDerivedViewMaterializer *materializer;
    duckdb_database database;
    duckdb_connection connection;
    guint batch_size;
    guint64 loaded_fact_count;
};

G_DEFINE_TYPE (WyreboxDaemonWirelogViews, wyrebox_daemon_wirelog_views,
    G_TYPE_OBJECT);

static void
duckdb_result_clear (duckdb_result *result)
{
    duckdb_destroy_result (result);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_result, duckdb_result_clear)

static void
duckdb_prepared_statement_clear (duckdb_prepared_statement *statement)
{
    duckdb_destroy_prepare (statement);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_prepared_statement,
    duckdb_prepared_statement_clear)

static void
duckdb_appender_cleanup (duckdb_appender *appender)
{
    if (*appender != NULL)
        (void)duckdb_appender_destroy (appender);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_appender, duckdb_appender_cleanup)

static void
duckdb_error_data_clear (duckdb_error_data *error_data)
{
    duckdb_destroy_error_data (error_data);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (duckdb_error_data, duckdb_error_data_clear)

static void
wirelog_view_free (gpointer data)
{
    WirelogView *view = data;

    g_free (view->view_id);
    g_free (view->imap_name);
    g_free (view->definition_ref);
    g_free (view);
}

static void
thread_link_free (gpointer data)
{
    ThreadLink *link = data;

    g_free (link->message_id);
    g_free (link->link_key);
    g_free (link);
}

static void
fact_record_ptr_free (gpointer data)
{
    WyreboxFactRecord *record = data;

    wyrebox_fact_record_clear (record);
    g_free (record);
}

static void
wyrebox_daemon_wirelog_views_dispose (GObject *object)
{
    WyreboxDaemonWirelogViews *self = WYREBOX_DAEMON_WIRELOG_VIEWS (object);

    g_clear_object (&self->materializer);
    g_clear_object (&self->journal_writer);

    G_OBJECT_CLASS (wyrebox_daemon_wirelog_views_parent_class)->dispose
        (object);
}

static void
wyrebox_daemon_wirelog_views_finalize (GObject *object)
{
    WyreboxDaemonWirelogViews *self = WYREBOX_DAEMON_WIRELOG_VIEWS (object);

    if (self->connection != NULL)
        duckdb_disconnect (&self->connection);
    if (self->database != NULL)
        duckdb_close (&self->database);
    g_clear_pointer (&self->rules_path, g_free);
    g_clear_pointer (&self->rules_source, g_free);
    g_clear_pointer (&self->rule_version_hash, g_free);
    g_clear_pointer (&self->declared_predicates, g_hash_table_unref);
    g_clear_pointer (&self->views, g_ptr_array_unref);
    g_clear_pointer (&self->known_symbols, g_ptr_array_unref);

    G_OBJECT_CLASS (wyrebox_daemon_wirelog_views_parent_class)->finalize
        (object);
}

static void
wyrebox_daemon_wirelog_views_class_init (WyreboxDaemonWirelogViewsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = wyrebox_daemon_wirelog_views_dispose;
    object_class->finalize = wyrebox_daemon_wirelog_views_finalize;
}

static void
wyrebox_daemon_wirelog_views_init (WyreboxDaemonWirelogViews *self)
{
    self->declared_predicates = g_hash_table_new_full (g_str_hash,
            g_str_equal, g_free, NULL);
    self->views = g_ptr_array_new_with_free_func (wirelog_view_free);
    self->known_symbols = g_ptr_array_new ();
    g_ptr_array_add (self->known_symbols, NULL);
    self->batch_size = WYREBOX_DAEMON_WIRELOG_VIEWS_DEFAULT_BATCH_SIZE;
}

/*
 * Facts whose predicate the rules do not declare cannot be loaded into the
 * program, so only declared predicates are read from message_facts.
 */
static void
collect_declared_predicates (const char *rules_source, GHashTable *predicates)
{
    g_auto (GStrv) lines = g_strsplit (rules_source, "\n", -1);

    for (guint i = 0; lines[i] != NULL; i++) {
        const char *cursor = lines[i];
        const char *start = NULL;

        while (g_ascii_isspace (*cursor))
            cursor++;
        if (!g_str_has_prefix (cursor, ".decl"))
            continue;
        cursor += strlen (".decl");
        if (!g_ascii_isspace (*cursor))
            continue;
        while (g_ascii_isspace (*cursor))
            cursor++;

        start = cursor;
        while (g_ascii_isalnum (*cursor) || *cursor == '_')
            cursor++;
        if (cursor > start)
            g_hash_table_add (predicates, g_strndup (start, cursor - start));
    }
}

WyreboxDaemonWirelogViews *
wyrebox_daemon_wirelog_views_new (const char *rules_path, GError **error)
{
    g_autoptr (WyreboxDaemonWirelogViews) self = NULL;
    g_autoptr (WyreboxWirelogProgram) program = NULL;
    g_autoptr (GError) local_error = NULL;
    g_autofree char *source = NULL;

    g_return_val_if_fail (rules_path != NULL, NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    if (!g_file_get_contents (rules_path, &source, NULL, &local_error)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "Wirelog rules file '%s' cannot be read: %s", rules_path,
            local_error->message);
        return NULL;
    }

    program = wyrebox_wirelog_program_new_from_source (source, &local_error);
    if (program == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "Wirelog rules file '%s' does not compile: %s", rules_path,
            local_error->message);
        return NULL;
    }

    self = g_object_new (WYREBOX_TYPE_DAEMON_WIRELOG_VIEWS, NULL);
    self->rule_version_hash = wyrebox_wirelog_rule_version_hash (source,
            &local_error);
    if (self->rule_version_hash == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "Wirelog rules file '%s' cannot be versioned: %s", rules_path,
            local_error->message);
        return NULL;
    }

    self->rules_path = g_strdup (rules_path);
    collect_declared_predicates (source, self->declared_predicates);
    self->rules_source = g_steal_pointer (&source);

    return g_steal_pointer (&self);
}

gboolean
wyrebox_daemon_wirelog_views_add_view (WyreboxDaemonWirelogViews *self,
    const char *view_id, const char *imap_name, WyreboxDaemonViewScope scope,
    GError **error)
{
    WirelogView *view = NULL;

    g_return_val_if_fail (WYREBOX_IS_DAEMON_WIRELOG_VIEWS (self), FALSE);
    g_return_val_if_fail (view_id != NULL && imap_name != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (!g_hash_table_contains (self->declared_predicates,
        WYREBOX_DAEMON_WIRELOG_VIEWS_MEMBERSHIP_RELATION)) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_INVALID_DATA,
            "Wirelog rules file '%s' must declare "
            WYREBOX_DAEMON_WIRELOG_VIEWS_MEMBERSHIP_RELATION
            "(view_id: symbol, message_id: symbol) for view '%s'",
            self->rules_path, view_id);
        return FALSE;
    }

    view = g_new0 (WirelogView, 1);
    view->view_id = g_strdup (view_id);
    view->imap_name = g_strdup (imap_name);
    view->definition_ref = g_strdup_printf ("wirelog:%s", view_id);
    view->scope = scope;
    g_ptr_array_add (self->views, view);

    g_ptr_array_insert (self->known_symbols, self->known_symbols->len - 1,
        view->view_id);
    return TRUE;
}

gboolean
wyrebox_daemon_wirelog_views_open_catalog (WyreboxDaemonWirelogViews *self,
    const char *catalog_path, WyreboxJournalWriter *journal_writer,
    GError **error)
{
    g_return_val_if_fail (WYREBOX_IS_DAEMON_WIRELOG_VIEWS (self), FALSE);
    g_return_val_if_fail (catalog_path != NULL, FALSE);
    g_return_val_if_fail (WYREBOX_IS_JOURNAL_WRITER (journal_writer), FALSE);
    g_return_val_if_fail (self->connection == NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    self->materializer = wyrebox_derived_view_materializer_new_duckdb
            (catalog_path, error);
    if (self->materializer == NULL)
        return FALSE;

    if (!wyrebox_duckdb_open_shared (catalog_path, &self->database, error))
        return FALSE;

    if (duckdb_connect (self->database, &self->connection) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "failed to connect to DuckDB catalog '%s'", catalog_path);
        return FALSE;
    }

    self->journal_writer = g_object_ref (journal_writer);
    return TRUE;
}

static gboolean
views_prepare (WyreboxDaemonWirelogViews *self, const char *sql,
    duckdb_prepared_statement *out_statement, GError **error)
{
    if (self->connection == NULL) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_NOT_INITIALIZED,
            "Wirelog views catalog is not open");
        return FALSE;
    }

    if (duckdb_prepare (self->connection, sql, out_statement) == DuckDBSuccess)
        return TRUE;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "failed to prepare Wirelog views query: %s",
        *out_statement != NULL ?
        duckdb_prepare_error (*out_statement) : "unknown DuckDB error");
    return FALSE;
}

static gboolean
views_execute (duckdb_prepared_statement statement, duckdb_result *result,
    GError **error)
{
    if (duckdb_execute_prepared (statement, result) == DuckDBSuccess)
        return TRUE;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "failed to execute Wirelog views query: %s",
        duckdb_result_error (result));
    return FALSE;
}

static char *
result_string (duckdb_result *result, idx_t column, idx_t row)
{
    char *value = duckdb_value_varchar (result, column, row);
    char *copy = g_strdup (value != NULL ? value : "");

    duckdb_free (value);
    return copy;
}

GStrv
wyrebox_daemon_wirelog_views_list_accounts (WyreboxDaemonWirelogViews *self,
    GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
    idx_t rows = 0;

    g_return_val_if_fail (WYREBOX_IS_DAEMON_WIRELOG_VIEWS (self), NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    if (!views_prepare (self,
        "SELECT account_id FROM accounts ORDER BY account_id;", &statement,
        error) ||
        !views_execute (statement, &result, error))
        return NULL;

    rows = duckdb_row_count (&result);
    for (idx_t row = 0; row < rows; row++)
        g_strv_builder_take (builder, result_string (&result, 0, row));

    return g_strv_builder_end (builder);
}

static gboolean
parse_json_hex (const char *cursor, gunichar *out_value)
{
    gunichar value = 0;

    for (guint i = 0; i < 4; i++) {
        gint digit = g_ascii_xdigit_value (cursor[i]);

        if (digit < 0)
            return FALSE;
        value = value * 16 + (gunichar)digit;
    }

    *out_value = value;
    return TRUE;
}

/*
 * Parses the JSON array of strings that the delivery materializer stores in
 * message_facts.args_json.
 */
static GStrv
parse_args_json (const char *json, GError **error)
{
    g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();
    const char *cursor = json;
    gboolean expect_value = TRUE;

    if (*cursor++ != '[')
        goto malformed;

    if (*cursor == ']') {
        if (cursor[1] != '\0')
            goto malformed;
        return g_strv_builder_end (builder);
    }

    while (expect_value) {
        g_autoptr (GString) value = g_string_new (NULL);

        if (*cursor++ != '"')
            goto malformed;

        while (*cursor != '"') {
            gunichar code = 0;

            if (*cursor == '\0' || (guchar)*cursor < 0x20)
                goto malformed;
            if (*cursor != '\\') {
                g_string_append_c (value, *cursor++);
                continue;
            }

            cursor++;
            switch (*cursor) {
            case '"':
            case '\\':
            case '/':
                g_string_append_c (value, *cursor);
                break;
            case 'b':
                g_string_append_c (value, '\b');
                break;
            case 'f':
                g_string_append_c (value, '\f');
                break;
            case 'n':
                g_string_append_c (value, '\n');
                break;
            case 'r':
                g_string_append_c (value, '\r');
                break;
            case 't':
                g_string_append_c (value, '\t');
                break;
            case 'u':
                if (!parse_json_hex (cursor + 1, &code) ||
                    (code >= 0xd800 && code <= 0xdfff))
                    goto malformed;
                g_string_append_unichar (value, code);
                cursor += 4;
                break;
            default:
                goto malformed;
            }
            cursor++;
        }
        cursor++;

        g_strv_builder_add (builder, value->str);
        if (*cursor == ',') {
            cursor++;
        } else if (*cursor == ']' && cursor[1] == '\0') {
            expect_value = FALSE;
        } else {
            goto malformed;
        }
    }

    return g_strv_builder_end (builder);

malformed:
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "message_facts args_json is not a JSON array of strings: %s", json);
    return NULL;
}

/*
 * Appends the facts of a predicate, args_json, source, confidence_ppm,
 * created_at_unix_us result whose predicate the rules declare.
 */
static gboolean
append_fact_rows (WyreboxDaemonWirelogViews *self, duckdb_result *result,
    GPtrArray *facts, GError **error)
{
    idx_t rows = duckdb_row_count (result);

    for (idx_t row = 0; row < rows; row++) {
        g_autofree char *predicate = result_string (result, 0, row);
        g_autofree char *args_json = NULL;
        g_autofree char *source = NULL;
        g_auto (GStrv) args = NULL;
        WyreboxFactRecord *record = NULL;

        if (!g_hash_table_contains (self->declared_predicates, predicate))
            continue;

        args_json = result_string (result, 1, row);
        source = result_string (result, 2, row);
        args = parse_args_json (args_json, error);
        if (args == NULL)
            return FALSE;

        record = g_new0 (WyreboxFactRecord, 1);
        if (!wyrebox_fact_record_init (record, predicate,
            (const char *const *)args, source,
            (guint32)duckdb_value_uint64 (result, 3, row),
            duckdb_value_uint64 (result, 4, row), error)) {
            fact_record_ptr_free (record);
            return FALSE;
        }
        g_ptr_array_add (facts, record);
    }

    return TRUE;
}

static GPtrArray *
load_account_facts (WyreboxDaemonWirelogViews *self, const char *account_id,
    GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    g_autoptr (GPtrArray) facts =
        g_ptr_array_new_with_free_func (fact_record_ptr_free);

    if (!views_prepare (self,
        "SELECT predicate, args_json, source, confidence_ppm, "
        "created_at_unix_us FROM message_facts "
        "WHERE account_id = ? AND retracted_at_unix_us = 0 "
        "ORDER BY fact_id;", &statement, error))
        return NULL;

    if (duckdb_bind_varchar (statement, 1, account_id) != DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED, "failed to bind Wirelog views account");
        return NULL;
    }

    if (!views_execute (statement, &result, error) ||
        !append_fact_rows (self, &result, facts, error))
        return NULL;

    return g_steal_pointer (&facts);
}

GPtrArray *
wyrebox_daemon_wirelog_views_evaluate (WyreboxDaemonWirelogViews *self,
    const char *account_id, const char *relation_name, GError **error)
{
    g_autoptr (GPtrArray) facts = NULL;

    g_return_val_if_fail (WYREBOX_IS_DAEMON_WIRELOG_VIEWS (self), NULL);
    g_return_val_if_fail (account_id != NULL && relation_name != NULL, NULL);
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);

    facts = load_account_facts (self, account_id, error);
    if (facts == NULL)
        return NULL;

    return
        wyrebox_wirelog_derived_membership_snapshot_from_rules_facts_and_symbols
            (self->rules_source, facts,
            (const char *const *)self->known_symbols->pdata, relation_name,
            error);
}

static GPtrArray *
memberships_of_existing_messages (WyreboxDaemonWirelogViews *self,
    const char *account_id, const char *view_id, GPtrArray *memberships,
    GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_autoptr (GPtrArray) selected = g_ptr_array_new ();

    if (!views_prepare (self,
        "SELECT COUNT(*) FROM messages WHERE account_id = ? "
        "AND message_id = ?;", &statement, error))
        return NULL;

    for (guint i = 0; i < memberships->len; i++) {
        WyreboxWirelogDerivedMembership *membership =
            g_ptr_array_index (memberships, i);
        g_auto (duckdb_result) result = { 0 };

        if (g_strcmp0 (membership->view_id, view_id) != 0)
            continue;

        if (duckdb_bind_varchar (statement, 1, account_id) != DuckDBSuccess ||
            duckdb_bind_varchar (statement, 2,
            membership->message_id) != DuckDBSuccess) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED, "failed to bind Wirelog views message");
            return NULL;
        }

        if (!views_execute (statement, &result, error))
            return NULL;

        if (duckdb_value_uint64 (&result, 0, 0) > 0)
            g_ptr_array_add (selected, membership);
    }

    return g_steal_pointer (&selected);
}

static gboolean
bind_uint64 (duckdb_prepared_statement statement, idx_t index, guint64 value,
    GError **error)
{
    if (duckdb_bind_uint64 (statement, index, value) == DuckDBSuccess)
        return TRUE;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "failed to bind Wirelog views sequence");
    return FALSE;
}

static gboolean
bind_varchar (duckdb_prepared_statement statement, idx_t index,
    const char *value, GError **error)
{
    if (duckdb_bind_varchar (statement, index, value) == DuckDBSuccess)
        return TRUE;

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED, "failed to bind Wirelog views value");
    return FALSE;
}

static void
set_appender_error (duckdb_appender appender, const char *operation,
    GError **error)
{
    g_auto (duckdb_error_data) error_data =
        duckdb_appender_error_data (appender);
    const char *message = duckdb_error_data_message (error_data);

    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "%s: %s",
        operation,
        message != NULL ? message : "unknown DuckDB error");
}

/*
 * This connection-local scratch table is replaced before every thread-scope
 * refresh. If a refresh fails, the next attempt replaces it before reading it.
 */
static gboolean
fill_thread_links (WyreboxDaemonWirelogViews *self, const char *account_id,
    gboolean full, guint64 since, GError **error)
{
    g_autoptr (GPtrArray) links = g_ptr_array_new_with_free_func (
        thread_link_free);

    {
        g_auto (duckdb_prepared_statement) statement = NULL;
        g_auto (duckdb_result) result = { 0 };

        if (!views_prepare (self,
            "SELECT message_id, args_json FROM message_facts "
            "WHERE account_id = ? "
            "AND predicate IN ('message_id', 'replies_to', 'references') "
            "AND (retracted_at_unix_us = 0 OR journal_sequence > ?) "
            "ORDER BY fact_id;", &statement, error) ||
            !bind_varchar (statement, 1, account_id, error) ||
            !bind_uint64 (statement, 2, full ? G_MAXUINT64 : since, error) ||
            !views_execute (statement, &result, error))
            return FALSE;

        for (idx_t row = 0; row < duckdb_row_count (&result); row++) {
            g_autofree char *message_id = result_string (&result, 0, row);
            g_autofree char *args_json = result_string (&result, 1, row);
            g_auto (GStrv) args = parse_args_json (args_json, error);
            ThreadLink *link = NULL;

            if (args == NULL)
                return FALSE;
            if (message_id == NULL || args[0] == NULL || args[1] == NULL)
                continue;

            link = g_new0 (ThreadLink, 1);
            link->message_id = g_steal_pointer (&message_id);
            link->link_key = g_strdup (args[1]);
            g_ptr_array_add (links, link);
        }
    }

    {
        g_auto (duckdb_result) result = { 0 };

        if (duckdb_query (self->connection,
            "CREATE OR REPLACE TEMP TABLE wirelog_thread_links ("
            "message_id VARCHAR, link_key VARCHAR);", &result) !=
            DuckDBSuccess) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "failed to create Wirelog thread links: %s",
                duckdb_result_error (&result));
            return FALSE;
        }
    }

    {
        g_auto (duckdb_appender) appender = NULL;

        if (duckdb_appender_create_ext (self->connection, "temp", "main",
            "wirelog_thread_links", &appender) != DuckDBSuccess) {
            set_appender_error (appender,
                "failed to create Wirelog thread link appender", error);
            return FALSE;
        }

        for (guint i = 0; i < links->len; i++) {
            const ThreadLink *link = g_ptr_array_index (links, i);

            if (duckdb_append_varchar (appender, link->message_id) !=
                DuckDBSuccess ||
                duckdb_append_varchar (appender, link->link_key) !=
                DuckDBSuccess ||
                duckdb_appender_end_row (appender) != DuckDBSuccess) {
                set_appender_error (appender,
                    "failed to append Wirelog thread links", error);
                return FALSE;
            }
        }

        if (duckdb_appender_flush (appender) != DuckDBSuccess) {
            set_appender_error (appender,
                "failed to flush Wirelog thread links", error);
            return FALSE;
        }
    }

    return TRUE;
}

static const char *
view_scope_name (WyreboxDaemonViewScope scope)
{
    switch (scope) {
    case WYREBOX_DAEMON_VIEW_SCOPE_MESSAGE:
        return "message";
    case WYREBOX_DAEMON_VIEW_SCOPE_THREAD:
        return "thread";
    case WYREBOX_DAEMON_VIEW_SCOPE_ACCOUNT:
    default:
        return "account";
    }
}

/*
 * Identifies the rules and views a refresh evaluated, so that a change to
 * either makes the next refresh evaluate every message again.
 */
static char *
refresh_config_hash (WyreboxDaemonWirelogViews *self)
{
    g_autoptr (GChecksum) checksum = g_checksum_new (G_CHECKSUM_SHA256);

    g_checksum_update (checksum, (const guchar *)self->rule_version_hash,
        strlen (self->rule_version_hash) + 1);
    for (guint i = 0; i < self->views->len; i++) {
        const WirelogView *view = g_ptr_array_index (self->views, i);
        const char *fields[] = {
            view->view_id,
            view->imap_name,
            view_scope_name (view->scope),
        };

        for (guint j = 0; j < G_N_ELEMENTS (fields); j++)
            g_checksum_update (checksum, (const guchar *)fields[j],
                strlen (fields[j]) + 1);
    }

    return g_strdup_printf ("sha256:%s", g_checksum_get_string (checksum));
}

static gboolean
read_refresh_state (WyreboxDaemonWirelogViews *self, const char *account_id,
    char **out_config_hash, guint64 *out_sequence, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };

    *out_config_hash = NULL;
    *out_sequence = 0;
    if (!views_prepare (self,
        "SELECT refresh_config_hash, refreshed_journal_sequence "
        "FROM derived_view_refresh_state WHERE account_id = ?;", &statement,
        error) ||
        !bind_varchar (statement, 1, account_id, error) ||
        !views_execute (statement, &result, error))
        return FALSE;

    if (duckdb_row_count (&result) == 0)
        return TRUE;

    *out_config_hash = result_string (&result, 0, 0);
    *out_sequence = duckdb_value_uint64 (&result, 1, 0);
    return TRUE;
}

static gboolean
write_refresh_state (WyreboxDaemonWirelogViews *self, const char *account_id,
    const char *config_hash, guint64 sequence, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };

    return views_prepare (self,
               "INSERT OR REPLACE INTO derived_view_refresh_state ("
               "account_id, refresh_config_hash, refreshed_journal_sequence"
               ") VALUES (?, ?, ?);", &statement, error)
           && bind_varchar (statement, 1, account_id, error)
           && bind_varchar (statement, 2, config_hash, error)
           && bind_uint64 (statement, 3, sequence, error)
           && views_execute (statement, &result, error);
}

/*
 * The newest journal sequence of the account's materialized messages and
 * facts; a fact insert or retract stores its own sequence on the fact row.
 */
static gboolean
read_account_sequence (WyreboxDaemonWirelogViews *self,
    const char *account_id, guint64 *out_sequence, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };

    if (!views_prepare (self,
        "SELECT GREATEST("
        "(SELECT COALESCE(MAX(journal_sequence), 0) FROM messages "
        "WHERE account_id = $1), "
        "(SELECT COALESCE(MAX(journal_sequence), 0) FROM message_facts "
        "WHERE account_id = $1));", &statement, error) ||
        !bind_varchar (statement, 1, account_id, error) ||
        !views_execute (statement, &result, error))
        return FALSE;

    *out_sequence = duckdb_value_uint64 (&result, 0, 0);
    return TRUE;
}

/*
 * Messages delivered, or whose facts were inserted or retracted, after
 * journal sequence $2.
 */
#define REFRESH_CHANGED_MESSAGES \
        "SELECT message_id FROM messages " \
        "WHERE account_id = $1 AND journal_sequence > $2 " \
        "UNION SELECT message_id FROM message_facts " \
        "WHERE account_id = $1 AND journal_sequence > $2"

#define REFRESH_EXISTING_MESSAGE \
        "message_id IN (SELECT message_id FROM messages WHERE account_id = $1)"

/*
 * Fills wirelog_refresh_scope with (component, message_id) rows, where a
 * component is the unit a batch never splits.
 */
static const char message_scope_sql[] =
    "CREATE OR REPLACE TEMP TABLE wirelog_refresh_scope AS "
    "SELECT message_id AS component, message_id FROM ("
    REFRESH_CHANGED_MESSAGES ") WHERE " REFRESH_EXISTING_MESSAGE ";";

/*
 * Messages sharing a message_id, replies_to, or references value are
 * connected. Links retracted since the previous refresh still connect, so
 * the messages a retraction disconnects are evaluated again. Each message's
 * component is the smallest changed message it is connected to.
 */
static const char thread_scope_sql[] =
    "CREATE OR REPLACE TEMP TABLE wirelog_refresh_scope AS "
    "WITH RECURSIVE link_keys AS ("
    "SELECT DISTINCT message_id, link_key FROM wirelog_thread_links), "
    "links AS (SELECT a.message_id AS source, b.message_id AS target "
    "FROM link_keys a JOIN link_keys b ON a.link_key = b.link_key "
    "AND a.message_id <> b.message_id), "
    "reach (seed, message_id) AS ("
    "SELECT message_id, message_id FROM (" REFRESH_CHANGED_MESSAGES ") "
    "UNION SELECT reach.seed, links.target FROM reach "
    "JOIN links ON links.source = reach.message_id) "
    "SELECT MIN(seed) AS component, message_id FROM reach "
    "WHERE " REFRESH_EXISTING_MESSAGE " GROUP BY message_id;";

static gboolean
fill_refresh_scope (WyreboxDaemonWirelogViews *self, const char *account_id,
    WyreboxDaemonViewScope scope, gboolean full, guint64 since,
    GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_auto (duckdb_result) result = { 0 };
    gboolean thread = scope == WYREBOX_DAEMON_VIEW_SCOPE_THREAD;

    if (thread && !fill_thread_links (self, account_id, full, since, error))
        return FALSE;

    return views_prepare (self, thread ? thread_scope_sql : message_scope_sql,
               &statement, error)
           && bind_varchar (statement, 1, account_id, error)
           && bind_uint64 (statement, 2, full ? 0 : since, error)
           && views_execute (statement, &result, error);
}

/*
 * Stores in @batch the messages of the components after @cursor, up to the
 * batch size plus the rest of the last component, and advances @cursor.
 */
static gboolean
next_refresh_batch (WyreboxDaemonWirelogViews *self, char **cursor,
    GPtrArray *batch, GError **error)
{
    g_auto (duckdb_prepared_statement) page = NULL;
    g_auto (duckdb_prepared_statement) rest = NULL;
    g_auto (duckdb_result) page_result = { 0 };
    g_auto (duckdb_result) rest_result = { 0 };
    idx_t rows = 0;

    if (!views_prepare (self,
        "SELECT component, message_id FROM wirelog_refresh_scope "
        "WHERE component > ? ORDER BY component, message_id LIMIT ?;",
        &page, error) ||
        !bind_varchar (page, 1, *cursor, error) ||
        !bind_uint64 (page, 2, self->batch_size, error) ||
        !views_execute (page, &page_result, error))
        return FALSE;

    rows = duckdb_row_count (&page_result);
    if (rows == 0)
        return TRUE;

    for (idx_t row = 0; row < rows; row++)
        g_ptr_array_add (batch, result_string (&page_result, 1, row));

    g_free (*cursor);
    *cursor = result_string (&page_result, 0, rows - 1);
    if (!views_prepare (self,
        "SELECT message_id FROM wirelog_refresh_scope "
        "WHERE component = ? AND message_id > ? ORDER BY message_id;",
        &rest, error) ||
        !bind_varchar (rest, 1, *cursor, error) ||
        !bind_varchar (rest, 2, g_ptr_array_index (batch, batch->len - 1),
        error) ||
        !views_execute (rest, &rest_result, error))
        return FALSE;

    for (idx_t row = 0; row < duckdb_row_count (&rest_result); row++)
        g_ptr_array_add (batch, result_string (&rest_result, 0, row));
    return TRUE;
}

static GPtrArray *
load_batch_facts (WyreboxDaemonWirelogViews *self, const char *account_id,
    GPtrArray *batch, GError **error)
{
    g_auto (duckdb_prepared_statement) statement = NULL;
    g_autoptr (GPtrArray) facts =
        g_ptr_array_new_with_free_func (fact_record_ptr_free);

    if (!views_prepare (self,
        "SELECT predicate, args_json, source, confidence_ppm, "
        "created_at_unix_us FROM message_facts "
        "WHERE account_id = ? AND message_id = ? "
        "AND retracted_at_unix_us = 0 ORDER BY fact_id;", &statement, error))
        return NULL;

    for (guint i = 0; i < batch->len; i++) {
        g_auto (duckdb_result) result = { 0 };

        if (!bind_varchar (statement, 1, account_id, error) ||
            !bind_varchar (statement, 2, g_ptr_array_index (batch, i), error) ||
            !views_execute (statement, &result, error) ||
            !append_fact_rows (self, &result, facts, error))
            return NULL;
    }

    self->loaded_fact_count += facts->len;
    return g_steal_pointer (&facts);
}

/*
 * Evaluates the rules over the facts of @batch and refreshes the membership
 * of those messages in each of @views.
 */
static gboolean
refresh_batch (WyreboxDaemonWirelogViews *self, const char *account_id,
    GPtrArray *views, GPtrArray *batch, guint64 now, GError **error)
{
    g_autoptr (GPtrArray) facts = NULL;
    g_autoptr (GPtrArray) memberships = NULL;
    g_autoptr (GHashTable) listed = g_hash_table_new (g_str_hash, g_str_equal);

    facts = load_batch_facts (self, account_id, batch, error);
    if (facts == NULL)
        return FALSE;

    memberships =
        wyrebox_wirelog_derived_membership_snapshot_from_rules_facts_and_symbols
            (self->rules_source, facts,
            (const char *const *)self->known_symbols->pdata,
            WYREBOX_DAEMON_WIRELOG_VIEWS_MEMBERSHIP_RELATION, error);
    if (memberships == NULL)
        return FALSE;

    for (guint i = 0; i < batch->len; i++)
        g_hash_table_add (listed, g_ptr_array_index (batch, i));
    g_ptr_array_add (batch, NULL);

    for (guint i = 0; i < views->len; i++) {
        const WirelogView *view = g_ptr_array_index (views, i);
        g_autoptr (GPtrArray) view_memberships = g_ptr_array_new ();
        g_autoptr (GPtrArray) changes = NULL;

        for (guint j = 0; j < memberships->len; j++) {
            WyreboxWirelogDerivedMembership *membership =
                g_ptr_array_index (memberships, j);

            if (g_strcmp0 (membership->view_id, view->view_id) == 0 &&
                g_hash_table_contains (listed, membership->message_id))
                g_ptr_array_add (view_memberships, membership);
        }

        if (!
            wyrebox_derived_view_materializer_refresh_message_memberships_with_changes
                (self->materializer, account_id, view->view_id, view->imap_name,
            view->definition_ref, self->rule_version_hash, now,
            (const char *const *)batch->pdata, view_memberships, &changes,
            error) ||
            !wyrebox_derived_view_membership_changes_append_journal (changes,
            self->journal_writer, error))
            return FALSE;
    }

    return TRUE;
}

static gboolean
refresh_scoped_views (WyreboxDaemonWirelogViews *self, const char *account_id,
    WyreboxDaemonViewScope scope, gboolean full, guint64 since, guint64 now,
    GError **error)
{
    g_autoptr (GPtrArray) views = g_ptr_array_new ();
    g_autofree char *cursor = g_strdup ("");
    g_autoptr (GPtrArray) none = g_ptr_array_new ();
    g_auto (duckdb_result) result = { 0 };

    for (guint i = 0; i < self->views->len; i++) {
        WirelogView *view = g_ptr_array_index (self->views, i);

        if (view->scope == scope)
            g_ptr_array_add (views, view);
    }
    if (views->len == 0)
        return TRUE;

    if (full && !refresh_batch (self, account_id, views, none, now, error))
        return FALSE;

    if (!fill_refresh_scope (self, account_id, scope, full, since, error))
        return FALSE;

    while (TRUE) {
        g_autoptr (GPtrArray) batch = g_ptr_array_new_with_free_func (g_free);

        if (!next_refresh_batch (self, &cursor, batch, error))
            return FALSE;
        if (batch->len == 0)
            break;
        if (!refresh_batch (self, account_id, views, batch, now, error))
            return FALSE;
    }

    if (duckdb_query (self->connection,
        "DROP TABLE IF EXISTS wirelog_refresh_scope;", &result) !=
        DuckDBSuccess) {
        g_set_error (error,
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            "failed to drop Wirelog views refresh scope: %s",
            duckdb_result_error (&result));
        return FALSE;
    }

    if (scope == WYREBOX_DAEMON_VIEW_SCOPE_THREAD) {
        g_auto (duckdb_result) thread_links_result = { 0 };

        if (duckdb_query (self->connection,
            "DROP TABLE IF EXISTS wirelog_thread_links;",
            &thread_links_result) != DuckDBSuccess) {
            g_set_error (error,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                "failed to drop Wirelog thread links: %s",
                duckdb_result_error (&thread_links_result));
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
refresh_account_views (WyreboxDaemonWirelogViews *self,
    const char *account_id, guint64 now, GError **error)
{
    g_autoptr (GPtrArray) facts = NULL;
    g_autoptr (GPtrArray) memberships = NULL;

    for (guint i = 0; i < self->views->len; i++) {
        const WirelogView *view = g_ptr_array_index (self->views, i);
        g_autoptr (GPtrArray) view_memberships = NULL;
        g_autoptr (GPtrArray) changes = NULL;

        if (view->scope != WYREBOX_DAEMON_VIEW_SCOPE_ACCOUNT)
            continue;

        if (memberships == NULL) {
            facts = load_account_facts (self, account_id, error);
            if (facts == NULL)
                return FALSE;
            self->loaded_fact_count += facts->len;
            memberships =
                wyrebox_wirelog_derived_membership_snapshot_from_rules_facts_and_symbols
                    (self->rules_source, facts,
                    (const char *const *)self->known_symbols->pdata,
                    WYREBOX_DAEMON_WIRELOG_VIEWS_MEMBERSHIP_RELATION, error);
            g_clear_pointer (&facts, g_ptr_array_unref);
            if (memberships == NULL)
                return FALSE;
        }

        view_memberships = memberships_of_existing_messages (self, account_id,
                view->view_id, memberships, error);
        if (view_memberships == NULL)
            return FALSE;

        if (!
            wyrebox_derived_view_materializer_refresh_current_rule_version_with_changes
                (self->materializer, account_id, view->view_id, view->imap_name,
            view->definition_ref, self->rule_version_hash, now,
            view_memberships, &changes, error) ||
            !wyrebox_derived_view_membership_changes_append_journal (changes,
            self->journal_writer, error))
            return FALSE;
    }

    return TRUE;
}

gboolean
wyrebox_daemon_wirelog_views_refresh_account (WyreboxDaemonWirelogViews *self,
    const char *account_id, GError **error)
{
    g_autofree char *config_hash = NULL;
    g_autofree char *refreshed_config_hash = NULL;
    guint64 refreshed_sequence = 0;
    guint64 latest_sequence = 0;
    guint64 now = (guint64)g_get_real_time ();
    gboolean full = FALSE;

    g_return_val_if_fail (WYREBOX_IS_DAEMON_WIRELOG_VIEWS (self), FALSE);
    g_return_val_if_fail (account_id != NULL, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    if (self->views->len == 0)
        return TRUE;

    config_hash = refresh_config_hash (self);
    if (!read_refresh_state (self, account_id, &refreshed_config_hash,
        &refreshed_sequence, error) ||
        !read_account_sequence (self, account_id, &latest_sequence, error))
        return FALSE;

    full = g_strcmp0 (refreshed_config_hash, config_hash) != 0;
    if (!full && latest_sequence <= refreshed_sequence)
        return TRUE;

    return refresh_scoped_views (self, account_id,
               WYREBOX_DAEMON_VIEW_SCOPE_MESSAGE, full, refreshed_sequence, now,
               error)
           && refresh_scoped_views (self, account_id,
               WYREBOX_DAEMON_VIEW_SCOPE_THREAD, full, refreshed_sequence, now,
               error)
           && refresh_account_views (self, account_id, now, error)
           && write_refresh_state (self, account_id, config_hash,
               latest_sequence, error);
}

void
wyrebox_daemon_wirelog_views_set_batch_size (WyreboxDaemonWirelogViews *self,
    guint batch_size)
{
    g_return_if_fail (WYREBOX_IS_DAEMON_WIRELOG_VIEWS (self));
    g_return_if_fail (batch_size > 0);

    self->batch_size = batch_size;
}

guint64
wyrebox_daemon_wirelog_views_get_loaded_fact_count (WyreboxDaemonWirelogViews
    *self)
{
    g_return_val_if_fail (WYREBOX_IS_DAEMON_WIRELOG_VIEWS (self), 0);

    return self->loaded_fact_count;
}

static GPtrArray *
predicate_query_evaluate (const char *account_id, const char *relation_name,
    gpointer user_data, GError **error)
{
    return wyrebox_daemon_wirelog_views_evaluate (user_data, account_id,
               relation_name, error);
}

WyreboxDaemonWirelogPredicateQueryService *
wyrebox_daemon_wirelog_views_new_predicate_query_service
    (WyreboxDaemonWirelogViews *self)
{
    g_return_val_if_fail (WYREBOX_IS_DAEMON_WIRELOG_VIEWS (self), NULL);

    return wyrebox_daemon_wirelog_predicate_query_service_new_with_evaluator
               (predicate_query_evaluate, g_object_ref (self), g_object_unref);
}
