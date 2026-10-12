#include "config.h"

#include "lib.h"

#include <stddef.h>
#include <string.h>

#include "array.h"
#include "imap-match.h"
#include "index-mail.h"
#include "index-storage.h"
#include "istream.h"
#include "mail-copy.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mail-storage-private.h"
#include "mail-user.h"
#include "mailbox-list-private.h"
#include "module-dir.h"
#include "wyrebox-daemon-mailbox-list-result.h"
#include "wyrebox-daemon-mailbox-select-result.h"
#include "wyrebox-dovecot-daemon-client.h"

#include <gio/gio.h>

#define WYREBOX_DOVECOT_LIST_NAME "wyrebox"
#define WYREBOX_DOVECOT_SOCKET_ENV "WYREBOX_SOCKET"
#define WYREBOX_DOVECOT_DEFAULT_SOCKET "/run/wyrebox/wyrebox.sock"
#define WYREBOX_DOVECOT_HIERARCHY_SEP '/'

struct wyrebox_dovecot_storage
{
    struct mail_storage storage;
    const char *socket_path;
    const char *account_identity;
};

typedef struct
{
    WyreboxDaemonMailboxSelectResult select_result;
    WyreboxDovecotMailboxUidMapSnapshot uid_map;
    /* UID -> row borrowed from uid_map; NULL until loaded. */
    GHashTable *rows_by_uid;
} WyreboxDovecotMailboxState;

struct wyrebox_dovecot_mailbox
{
    struct mailbox box;
    /* Daemon state matching the UIDs currently in the mailbox index. */
    WyreboxDovecotMailboxState state;
    gboolean inconsistent;
    /* Set while this mailbox holds an index sync; commits must not nest. */
    gboolean in_index_sync;
    /* Ordinary mailboxes accept flag changes; views stay read-only. */
    gboolean writable;
};

struct wyrebox_dovecot_transaction
{
    /* First member: index_transaction_commit() frees the whole struct. */
    struct mailbox_transaction_context t;
    gboolean flags_changed;
    gboolean expunge_refused;
};

struct wyrebox_dovecot_mailbox_list
{
    struct mailbox_list list;
    const char *socket_path;
};

struct wyrebox_dovecot_mailbox_list_iter
{
    struct mailbox_list_iterate_context ctx;
    struct mailbox_info *infos;
    unsigned int n_infos;
    unsigned int next_info;
};

static struct mail_storage wyrebox_mail_storage_class;
static struct mailbox_list wyrebox_mailbox_list_class;
static struct mailbox wyrebox_dovecot_mailbox_template;
static struct mail_vfuncs wyrebox_dovecot_mail_vfuncs;

static const char *
wyrebox_dovecot_socket_path (void)
{
    const char *socket_path = g_getenv (WYREBOX_DOVECOT_SOCKET_ENV);

    if (socket_path != NULL && socket_path[0] != '\0') {
        return socket_path;
    }

    return WYREBOX_DOVECOT_DEFAULT_SOCKET;
}

static struct mailbox_list *
wyrebox_dovecot_list_alloc (void)
{
    struct wyrebox_dovecot_mailbox_list *wlist;
    pool_t pool;

    pool = pool_alloconly_create ("wyrebox list", 1024);
    wlist = p_new (pool, struct wyrebox_dovecot_mailbox_list, 1);
    wlist->list = wyrebox_mailbox_list_class;
    wlist->list.pool = pool;
    wlist->socket_path = p_strdup (pool, wyrebox_dovecot_socket_path ());
    return &wlist->list;
}

static void
wyrebox_dovecot_list_deinit (struct mailbox_list *list)
{
    pool_unref (&list->pool);
}

static char
wyrebox_dovecot_list_get_hierarchy_sep (struct mailbox_list *list)
{
    (void)list;
    return WYREBOX_DOVECOT_HIERARCHY_SEP;
}

static int
wyrebox_dovecot_list_get_path (struct mailbox_list *list, const char *name,
    enum mailbox_list_path_type type, const char **path_r)
{
    (void)list;
    (void)name;
    (void)type;

    /* No path keeps every index in memory; the daemon owns mailbox state. */
    *path_r = NULL;
    return 0;
}

static const char *
wyrebox_dovecot_list_get_temp_prefix (struct mailbox_list *list,
    bool global)
{
    (void)list;
    (void)global;
    return ".temp.";
}

static int
wyrebox_dovecot_list_subscriptions_refresh (struct mailbox_list *src_list,
    struct mailbox_list *dest_list)
{
    (void)src_list;
    (void)dest_list;
    return 0;
}

static int
wyrebox_dovecot_list_set_subscribed (struct mailbox_list *list,
    const char *name, bool set)
{
    (void)name;
    (void)set;
    mailbox_list_set_error (list, MAIL_ERROR_NOTPOSSIBLE, "Not supported");
    return -1;
}

static int
wyrebox_dovecot_list_delete_mailbox (struct mailbox_list *list,
    const char *name)
{
    (void)name;
    mailbox_list_set_error (list, MAIL_ERROR_NOTPOSSIBLE, "Not supported");
    return -1;
}

static int
wyrebox_dovecot_list_rename_mailbox (struct mailbox_list *oldlist,
    const char *oldname, struct mailbox_list *newlist, const char *newname)
{
    (void)oldname;
    (void)newlist;
    (void)newname;
    mailbox_list_set_error (oldlist, MAIL_ERROR_NOTPOSSIBLE, "Not supported");
    return -1;
}

static int
wyrebox_dovecot_list_get_mailbox_flags (struct mailbox_list *list,
    const char *dir, const char *fname, enum mailbox_list_file_type type,
    enum mailbox_info_flags *flags_r)
{
    (void)list;
    (void)dir;
    (void)fname;
    (void)type;
    *flags_r = MAILBOX_NONEXISTENT;
    return 0;
}

static enum mailbox_info_flags
wyrebox_dovecot_list_info_flags (const WyreboxDaemonMailboxListEntry *entry,
    enum mailbox_list_iter_flags iter_flags)
{
    enum mailbox_info_flags flags = 0;

    if ((iter_flags & MAILBOX_LIST_ITER_RETURN_NO_FLAGS) != 0) {
        return 0;
    }

    if (!entry->is_selectable) {
        flags |= MAILBOX_NOSELECT;
    }

    switch (entry->child_state) {
    case WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN:
        flags |= MAILBOX_CHILDREN;
        break;
    case WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN:
        flags |= MAILBOX_NOCHILDREN;
        break;
    case WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN:
    default:
        break;
    }

    return flags;
}

static gboolean
wyrebox_dovecot_list_validate_entry (const WyreboxDaemonMailboxListEntry
    *entry, GError **error)
{
    if (entry == NULL || entry->mailbox_name == NULL
        || entry->mailbox_name[0] == '\0') {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "WyreBox daemon LIST returned an empty mailbox name");
        return FALSE;
    }

    if (entry->hierarchy_delimiter == NULL
        || entry->hierarchy_delimiter[0] != WYREBOX_DOVECOT_HIERARCHY_SEP
        || entry->hierarchy_delimiter[1] != '\0') {
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "WyreBox daemon LIST returned mailbox '%s' with a delimiter "
            "other than '%c'", entry->mailbox_name,
            WYREBOX_DOVECOT_HIERARCHY_SEP);
        return FALSE;
    }

    switch (entry->child_state) {
    case WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN:
    case WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN:
    case WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN:
        return TRUE;
    default:
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
            "WyreBox daemon LIST returned an unsupported child state");
        return FALSE;
    }
}

static gboolean
wyrebox_dovecot_list_fill (struct wyrebox_dovecot_mailbox_list_iter *iter,
    const WyreboxDaemonMailboxListResult *result, GError **error)
{
    struct mailbox_list *list = iter->ctx.list;
    pool_t pool = iter->ctx.pool;
    guint n_entries = wyrebox_daemon_mailbox_list_result_get_n_entries
            (result);
    gboolean inbox_listed = FALSE;

    iter->infos = p_new (pool, struct mailbox_info, n_entries);

    for (guint i = 0; i < n_entries; i++) {
        const WyreboxDaemonMailboxListEntry *entry =
            wyrebox_daemon_mailbox_list_result_get_entry (result, i);
        struct mailbox_info *info;
        const char *vname;

        if (!wyrebox_dovecot_list_validate_entry (entry, error)) {
            return FALSE;
        }

        vname = entry->mailbox_name;
        if (g_ascii_strcasecmp (vname, "INBOX") == 0) {
            if (inbox_listed) {
                continue;
            }
            inbox_listed = TRUE;
            vname = "INBOX";
        }

        if (imap_match (iter->ctx.glob, vname) != IMAP_MATCH_YES) {
            continue;
        }

        info = &iter->infos[iter->n_infos++];
        info->ns = list->ns;
        info->vname = p_strdup (pool, vname);
        info->flags = wyrebox_dovecot_list_info_flags (entry, iter->ctx.flags);
        /* "\Inbox" is not an RFC 6154 special-use flag. */
        if ((iter->ctx.flags & MAILBOX_LIST_ITER_RETURN_SPECIALUSE) != 0
            && entry->special_use != NULL
            && strcmp (entry->special_use, "\\Inbox") != 0) {
            info->special_use = p_strdup (pool, entry->special_use);
        }
    }

    return TRUE;
}

static gboolean
wyrebox_dovecot_error_is_unavailable (const GError *error)
{
    if (g_error_matches (error, WYREBOX_DOVECOT_DAEMON_CLIENT_ERROR,
        WYREBOX_DAEMON_ERROR_TEMPORARY_FAILURE)) {
        return TRUE;
    }

    return g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)
           || g_error_matches (error, G_IO_ERROR,
               G_IO_ERROR_CONNECTION_REFUSED)
           || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED)
           || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE)
           || g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
}

static struct mailbox_list_iterate_context *
wyrebox_dovecot_list_iter_init (struct mailbox_list *list,
    const char *const *patterns, enum mailbox_list_iter_flags flags)
{
    struct wyrebox_dovecot_mailbox_list *wlist =
        (struct wyrebox_dovecot_mailbox_list *)list;
    struct wyrebox_dovecot_mailbox_list_iter *iter;
    g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
    g_autoptr (GError) error = NULL;
    pool_t pool;

    pool = pool_alloconly_create ("wyrebox list iter", 1024);
    iter = p_new (pool, struct wyrebox_dovecot_mailbox_list_iter, 1);
    iter->ctx.pool = pool;
    iter->ctx.list = list;
    iter->ctx.flags = flags;
    iter->ctx.glob = imap_match_init_multiple (pool, patterns, TRUE,
            mail_namespace_get_sep (list->ns));
    array_create (&iter->ctx.module_contexts, pool, sizeof (void *), 5);

    if (!wyrebox_dovecot_daemon_client_list_mailboxes (wlist->socket_path,
        list->ns->user->username, "", &result, &error)
        || !wyrebox_dovecot_list_fill (iter, &result, &error)) {
        if (wyrebox_dovecot_error_is_unavailable (error)) {
            e_error (list->event, "WyreBox daemon LIST failed: %s",
                error->message);
            mailbox_list_set_error (list, MAIL_ERROR_UNAVAILABLE,
                "WyreBox daemon is unavailable");
        } else {
            mailbox_list_set_critical (list, "WyreBox daemon LIST failed: %s",
                error->message);
        }
        iter->ctx.failed = TRUE;
        iter->n_infos = 0;
    }

    return &iter->ctx;
}

static const struct mailbox_info *
wyrebox_dovecot_list_iter_next (struct mailbox_list_iterate_context *ctx)
{
    struct wyrebox_dovecot_mailbox_list_iter *iter =
        (struct wyrebox_dovecot_mailbox_list_iter *)ctx;

    if (iter->next_info >= iter->n_infos) {
        return NULL;
    }

    return &iter->infos[iter->next_info++];
}

static int
wyrebox_dovecot_list_iter_deinit (struct mailbox_list_iterate_context *ctx)
{
    int ret = ctx->failed ? -1 : 0;

    pool_unref (&ctx->pool);
    return ret;
}

static void
wyrebox_dovecot_mailbox_set_daemon_error (struct mailbox *box,
    const char *operation, const GError *error)
{
    if (g_error_matches (error, WYREBOX_DOVECOT_DAEMON_CLIENT_ERROR,
        WYREBOX_DAEMON_ERROR_NOT_FOUND)) {
        mail_storage_set_error (box->storage, MAIL_ERROR_NOTFOUND,
            T_MAIL_ERR_MAILBOX_NOT_FOUND (box->vname));
        return;
    }

    if (wyrebox_dovecot_error_is_unavailable (error)) {
        e_error (box->event, "WyreBox daemon %s failed: %s", operation,
            error->message);
        mail_storage_set_error (box->storage, MAIL_ERROR_UNAVAILABLE,
            "WyreBox daemon is unavailable");
        return;
    }

    mailbox_set_critical (box, "WyreBox daemon %s failed: %s", operation,
        error->message);
}

static void
wyrebox_dovecot_mailbox_state_clear (WyreboxDovecotMailboxState *state)
{
    g_clear_pointer (&state->rows_by_uid, g_hash_table_unref);
    wyrebox_dovecot_mailbox_uid_map_snapshot_clear (&state->uid_map);
    wyrebox_daemon_mailbox_select_result_clear (&state->select_result);
}

/* *INDENT-OFF* */
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (WyreboxDovecotMailboxState,
    wyrebox_dovecot_mailbox_state_clear)
/* *INDENT-ON* */

/* Takes ownership of everything in @state and leaves it empty. */
static void
wyrebox_dovecot_mailbox_install_state (struct wyrebox_dovecot_mailbox *wbox,
    WyreboxDovecotMailboxState *state)
{
    wyrebox_dovecot_mailbox_state_clear (&wbox->state);
    wbox->state = *state;
    *state = (WyreboxDovecotMailboxState) { 0 };
}

static gint
wyrebox_dovecot_compare_uid (gconstpointer a, gconstpointer b)
{
    guint32 uid_a = *(const guint32 *)a;
    guint32 uid_b = *(const guint32 *)b;

    return uid_a < uid_b ? -1 : (uid_a > uid_b ? 1 : 0);
}

static gboolean
wyrebox_dovecot_index_uid_map (const WyreboxDaemonMailboxSelectResult
    *select_result, const WyreboxDovecotMailboxUidMapSnapshot *uid_map,
    GHashTable *rows_by_uid, GError **error)
{
    for (guint i = 0; uid_map->rows != NULL && i < uid_map->rows->len; i++) {
        WyreboxDovecotMailboxUidMapRow *row =
            g_ptr_array_index (uid_map->rows, i);

        if (row->uid == 0 || row->uid >= select_result->uid_next) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "UID %" G_GUINT64_FORMAT " is outside 1..UIDNEXT-1 (%u)",
                row->uid, select_result->uid_next);
            return FALSE;
        }

        if (row->uid_validity != select_result->uid_validity) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "UID %" G_GUINT64_FORMAT " has a stale UIDVALIDITY", row->uid);
            return FALSE;
        }

        if (row->message_id == NULL || row->message_id[0] == '\0') {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "UID %" G_GUINT64_FORMAT " has no message id", row->uid);
            return FALSE;
        }

        if (!g_hash_table_insert (rows_by_uid,
            GUINT_TO_POINTER ((guint32)row->uid), row)) {
            g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "UID %" G_GUINT64_FORMAT " appears twice", row->uid);
            return FALSE;
        }
    }

    return TRUE;
}

static gboolean
wyrebox_dovecot_mailbox_load_state (struct wyrebox_dovecot_mailbox *wbox,
    WyreboxDovecotMailboxState *state, const char **operation_r,
    GError **error)
{
    struct wyrebox_dovecot_storage *storage =
        (struct wyrebox_dovecot_storage *)wbox->box.storage;

    *operation_r = "mailbox SELECT";
    if (!wyrebox_dovecot_daemon_client_select_mailbox (storage->socket_path,
        storage->account_identity, wbox->box.vname, &state->select_result,
        error)) {
        return FALSE;
    }

    *operation_r = "mailbox UID map";
    if (!wyrebox_dovecot_daemon_client_load_uid_map (storage->socket_path,
        storage->account_identity, state->select_result.mailbox_id,
        state->select_result.kind, state->select_result.uid_validity,
        &state->uid_map, error)) {
        return FALSE;
    }

    state->rows_by_uid = g_hash_table_new (g_direct_hash, g_direct_equal);
    return wyrebox_dovecot_index_uid_map (&state->select_result,
               &state->uid_map, state->rows_by_uid, error);
}

static const struct
{
    const char *name;
    enum mail_flags flag;
} wyrebox_dovecot_system_flags[] = {
    { "\\Seen", MAIL_SEEN },
    { "\\Answered", MAIL_ANSWERED },
    { "\\Flagged", MAIL_FLAGGED },
    { "\\Deleted", MAIL_DELETED },
    { "\\Draft", MAIL_DRAFT },
};

static gboolean
wyrebox_dovecot_system_flag_from_name (const char *name,
    enum mail_flags *flag_r)
{
    for (guint i = 0; i < G_N_ELEMENTS (wyrebox_dovecot_system_flags); i++) {
        if (g_ascii_strcasecmp (name,
            wyrebox_dovecot_system_flags[i].name) == 0) {
            *flag_r = wyrebox_dovecot_system_flags[i].flag;
            return TRUE;
        }
    }
    return FALSE;
}

/* Replaces the flags and keywords of @seq with the daemon's row. */
static void
wyrebox_dovecot_index_apply_row_flags (struct mailbox *box,
    struct mail_index_transaction *trans, uint32_t seq,
    const WyreboxDovecotMailboxUidMapRow *row)
{
    T_BEGIN {
        ARRAY_TYPE (const_string) keywords;
        struct mail_keywords *index_keywords;
        enum mail_flags flags = 0;

        t_array_init (&keywords, 4);
        for (guint i = 0; row->flags != NULL && row->flags[i] != NULL; i++) {
            const char *name = row->flags[i];
            enum mail_flags flag;

            if (name[0] != '\\') {
                array_push_back (&keywords, &name);
            } else if (wyrebox_dovecot_system_flag_from_name (name, &flag)) {
                flags |= flag;
            } else {
                e_warning (box->event, "Ignoring unknown WyreBox flag %s "
                    "on UID %" G_GUINT64_FORMAT, name, row->uid);
            }
        }
        array_append_zero (&keywords);

        mail_index_update_flags (trans, seq, MODIFY_REPLACE, flags);
        index_keywords = mail_index_keywords_create (box->index,
                array_front (&keywords));
        mail_index_update_keywords (trans, seq, MODIFY_REPLACE,
            index_keywords);
        mail_index_keywords_unref (&index_keywords);
    } T_END;
}

static void
wyrebox_dovecot_sync_apply_existing_flags (struct mailbox *box,
    const WyreboxDovecotMailboxState *state,
    struct mail_index_view *sync_view,
    struct mail_index_transaction *sync_trans)
{
    const struct mail_index_header *hdr = mail_index_get_header (sync_view);

    for (uint32_t seq = 1; seq <= hdr->messages_count; seq++) {
        const WyreboxDovecotMailboxUidMapRow *row;
        uint32_t uid;

        mail_index_lookup_uid (sync_view, seq, &uid);
        row = g_hash_table_lookup (state->rows_by_uid, GUINT_TO_POINTER (uid));
        if (row != NULL) {
            wyrebox_dovecot_index_apply_row_flags (box, sync_trans, seq, row);
        }
    }
}

static void
wyrebox_dovecot_sync_expunge_missing (const WyreboxDovecotMailboxState *state,
    struct mail_index_view *sync_view,
    struct mail_index_transaction *sync_trans)
{
    const struct mail_index_header *hdr = mail_index_get_header (sync_view);

    for (uint32_t seq = 1; seq <= hdr->messages_count; seq++) {
        uint32_t uid;

        mail_index_lookup_uid (sync_view, seq, &uid);
        if (!g_hash_table_contains (state->rows_by_uid,
            GUINT_TO_POINTER (uid))) {
            mail_index_expunge (sync_trans, seq);
        }
    }
}

static void
wyrebox_dovecot_sync_append_new (struct mailbox *box,
    const WyreboxDovecotMailboxState *state,
    struct mail_index_view *sync_view,
    struct mail_index_transaction *sync_trans)
{
    const struct mail_index_header *hdr = mail_index_get_header (sync_view);
    g_autoptr (GArray) uids = g_array_new (FALSE, FALSE, sizeof (guint32));
    GHashTableIter iter;
    gpointer key;
    uint32_t next_uid = hdr->next_uid;

    g_hash_table_iter_init (&iter, state->rows_by_uid);
    while (g_hash_table_iter_next (&iter, &key, NULL)) {
        guint32 uid = GPOINTER_TO_UINT (key);

        if (uid >= next_uid) {
            g_array_append_val (uids, uid);
        }
    }
    g_array_sort (uids, wyrebox_dovecot_compare_uid);

    for (guint i = 0; i < uids->len; i++) {
        guint32 uid = g_array_index (uids, guint32, i);
        uint32_t seq;

        mail_index_append (sync_trans, uid, &seq);
        wyrebox_dovecot_index_apply_row_flags (box, sync_trans, seq,
            g_hash_table_lookup (state->rows_by_uid, GUINT_TO_POINTER (uid)));
    }

    if (state->select_result.uid_next > next_uid) {
        uint32_t uid_next = state->select_result.uid_next;

        mail_index_update_header (sync_trans,
            offsetof (struct mail_index_header, next_uid), &uid_next,
            sizeof (uid_next), FALSE);
    }
}

static int
wyrebox_dovecot_mailbox_sync_index (struct wyrebox_dovecot_mailbox *wbox)
{
    struct mailbox *box = &wbox->box;
    struct mail_index_sync_ctx *sync_ctx;
    struct mail_index_view *sync_view;
    struct mail_index_transaction *sync_trans;
    struct mail_index_sync_rec sync_rec;
    const struct mail_index_header *hdr;
    g_auto (WyreboxDovecotMailboxState) state = { 0 };
    g_autoptr (GError) error = NULL;
    const char *operation = NULL;
    uint32_t uid_validity;
    int ret;

    if (!wyrebox_dovecot_mailbox_load_state (wbox, &state, &operation,
        &error)) {
        wyrebox_dovecot_mailbox_set_daemon_error (box, operation, error);
        return -1;
    }

    ret = mail_index_sync_begin (box->index, &sync_ctx, &sync_view,
            &sync_trans, index_storage_get_sync_flags (box) |
            MAIL_INDEX_SYNC_FLAG_FLUSH_DIRTY |
            MAIL_INDEX_SYNC_FLAG_AVOID_FLAG_UPDATES);
    if (ret <= 0) {
        if (ret < 0) {
            mailbox_set_index_error (box);
        }
        return ret;
    }

    hdr = mail_index_get_header (sync_view);
    uid_validity = state.select_result.uid_validity;
    if (hdr->uid_validity == 0) {
        mail_index_update_header (sync_trans,
            offsetof (struct mail_index_header, uid_validity), &uid_validity,
            sizeof (uid_validity), TRUE);
    } else if (hdr->uid_validity != uid_validity) {
        /* Keep the old state so FETCH never serves the new generation's
         * bytes under old UIDs; imap disconnects on is_inconsistent. */
        mailbox_set_critical (box,
            "WyreBox UIDVALIDITY changed during the session (%u -> %u)",
            hdr->uid_validity, uid_validity);
        mail_index_sync_rollback (&sync_ctx);
        wbox->inconsistent = TRUE;
        return -1;
    }

    wbox->in_index_sync = TRUE;
    wyrebox_dovecot_sync_expunge_missing (&state, sync_view, sync_trans);
    wyrebox_dovecot_sync_apply_existing_flags (box, &state, sync_view,
        sync_trans);
    wyrebox_dovecot_sync_append_new (box, &state, sync_view, sync_trans);

    /* Flag changes are pushed at transaction commit. Anything still pending
     * was not accepted by the daemon, and the daemon flags applied above
     * replace it. */
    while (mail_index_sync_next (sync_ctx, &sync_rec)) {
    }

    ret = mail_index_sync_commit (&sync_ctx);
    wbox->in_index_sync = FALSE;
    if (ret < 0) {
        mailbox_set_index_error (box);
        return -1;
    }

    wyrebox_dovecot_mailbox_install_state (wbox, &state);
    return 0;
}

/* Net flag and keyword change of one UID since the last push. */
typedef struct
{
    uint32_t seq;
    enum mail_flags add_flags;
    enum mail_flags remove_flags;
    /* Owned keyword names, used as sets. */
    GHashTable *add_keywords;
    GHashTable *remove_keywords;
} WyreboxDovecotFlagDelta;

static WyreboxDovecotFlagDelta *
wyrebox_dovecot_flag_delta_new (uint32_t seq)
{
    WyreboxDovecotFlagDelta *delta = g_new0 (WyreboxDovecotFlagDelta, 1);

    delta->seq = seq;
    delta->add_keywords = g_hash_table_new_full (g_str_hash, g_str_equal,
            g_free, NULL);
    delta->remove_keywords = g_hash_table_new_full (g_str_hash, g_str_equal,
            g_free, NULL);
    return delta;
}

static void
wyrebox_dovecot_flag_delta_free (gpointer data)
{
    WyreboxDovecotFlagDelta *delta = data;

    g_hash_table_unref (delta->add_keywords);
    g_hash_table_unref (delta->remove_keywords);
    g_free (delta);
}

static void
wyrebox_dovecot_flag_delta_add_record (WyreboxDovecotFlagDelta *delta,
    const struct mail_index_sync_rec *rec, const char *keyword)
{
    enum mail_flags add = rec->add_flags & MAIL_INDEX_FLAGS_MASK;
    enum mail_flags remove = rec->remove_flags & MAIL_INDEX_FLAGS_MASK;

    switch (rec->type) {
    case MAIL_INDEX_SYNC_TYPE_FLAGS:
        delta->add_flags = (delta->add_flags & ~remove) | add;
        delta->remove_flags = (delta->remove_flags & ~add) | remove;
        break;
    case MAIL_INDEX_SYNC_TYPE_KEYWORD_ADD:
        g_hash_table_remove (delta->remove_keywords, keyword);
        g_hash_table_add (delta->add_keywords, g_strdup (keyword));
        break;
    case MAIL_INDEX_SYNC_TYPE_KEYWORD_REMOVE:
        g_hash_table_remove (delta->add_keywords, keyword);
        g_hash_table_add (delta->remove_keywords, g_strdup (keyword));
        break;
    case MAIL_INDEX_SYNC_TYPE_EXPUNGE:
        break;
    }
}

/* Collects the pending index records per UID. Returns FALSE on records the
 * push cannot express. */
static gboolean
wyrebox_dovecot_collect_flag_deltas (struct mailbox *box,
    struct mail_index_sync_ctx *sync_ctx, struct mail_index_view *sync_view,
    GHashTable *deltas)
{
    const ARRAY_TYPE (keywords) *names = mail_index_get_keywords (box->index);
    struct mail_index_sync_rec rec;
    gboolean ok = TRUE;

    while (mail_index_sync_next (sync_ctx, &rec)) {
        const char *keyword = NULL;
        uint32_t seq1, seq2;

        if (rec.type == MAIL_INDEX_SYNC_TYPE_EXPUNGE) {
            mailbox_set_critical (box, "WyreBox cannot push an expunge");
            ok = FALSE;
            continue;
        }
        if (rec.type != MAIL_INDEX_SYNC_TYPE_FLAGS) {
            if (rec.keyword_idx >= array_count (names)) {
                mailbox_set_critical (box,
                    "WyreBox flag push found unknown keyword index %u",
                    rec.keyword_idx);
                ok = FALSE;
                continue;
            }
            keyword = array_idx_elem (names, rec.keyword_idx);
        }

        if (!mail_index_lookup_seq_range (sync_view, rec.uid1, rec.uid2, &seq1,
            &seq2)) {
            continue;
        }
        for (uint32_t seq = seq1; seq <= seq2; seq++) {
            WyreboxDovecotFlagDelta *delta;
            uint32_t uid;

            mail_index_lookup_uid (sync_view, seq, &uid);
            delta = g_hash_table_lookup (deltas, GUINT_TO_POINTER (uid));
            if (delta == NULL) {
                delta = wyrebox_dovecot_flag_delta_new (seq);
                g_hash_table_insert (deltas, GUINT_TO_POINTER (uid), delta);
            }
            wyrebox_dovecot_flag_delta_add_record (delta, &rec, keyword);
        }
    }

    return ok;
}

/* Borrowed, NULL-terminated system flag names for @flags. */
static GPtrArray *
wyrebox_dovecot_system_flag_names (enum mail_flags flags)
{
    GPtrArray *names = g_ptr_array_new ();

    for (guint i = 0; i < G_N_ELEMENTS (wyrebox_dovecot_system_flags); i++) {
        if ((flags & wyrebox_dovecot_system_flags[i].flag) != 0) {
            g_ptr_array_add (names,
                (gpointer)wyrebox_dovecot_system_flags[i].name);
        }
    }
    g_ptr_array_add (names, NULL);
    return names;
}

static gint
wyrebox_dovecot_compare_names (gconstpointer a, gconstpointer b)
{
    return strcmp (*(const char *const *)a, *(const char *const *)b);
}

/* Borrowed, sorted, NULL-terminated keyword names from @set. */
static GPtrArray *
wyrebox_dovecot_keyword_names (GHashTable *set)
{
    GPtrArray *names = g_ptr_array_new ();
    GHashTableIter iter;
    gpointer key;

    g_hash_table_iter_init (&iter, set);
    while (g_hash_table_iter_next (&iter, &key, NULL)) {
        g_ptr_array_add (names, key);
    }
    g_ptr_array_sort (names, wyrebox_dovecot_compare_names);
    g_ptr_array_add (names, NULL);
    return names;
}

static gboolean
wyrebox_dovecot_strv_contains (const char *const *names, const char *name)
{
    return names != NULL && g_strv_contains (names, name);
}

/* Applies one accepted SET or CLEAR to the cached daemon row. */
static void
wyrebox_dovecot_row_apply_update (WyreboxDovecotMailboxUidMapRow *row,
    WyreboxDaemonFlagKeywordUpdateMode mode, const char *const *system_flags,
    const char *const *keywords)
{
    gboolean set = mode == WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET;
    g_autoptr (GStrvBuilder) builder = g_strv_builder_new ();

    for (guint i = 0; i < G_N_ELEMENTS (wyrebox_dovecot_system_flags); i++) {
        const char *name = wyrebox_dovecot_system_flags[i].name;
        gboolean present = wyrebox_dovecot_strv_contains (
            (const char *const *)row->flags, name);

        if (wyrebox_dovecot_strv_contains (system_flags, name)) {
            present = set;
        }
        if (present) {
            g_strv_builder_add (builder, name);
        }
    }
    for (guint i = 0; row->flags != NULL && row->flags[i] != NULL; i++) {
        const char *name = row->flags[i];

        if (name[0] == '\\') {
            continue;
        }
        if (!set && wyrebox_dovecot_strv_contains (keywords, name)) {
            continue;
        }
        g_strv_builder_add (builder, name);
    }
    for (guint i = 0; set && keywords[i] != NULL; i++) {
        if (!wyrebox_dovecot_strv_contains ((const char *const *)row->flags,
            keywords[i])) {
            g_strv_builder_add (builder, keywords[i]);
        }
    }

    g_strfreev (row->flags);
    row->flags = g_strv_builder_end (builder);
}

typedef enum
{
    WYREBOX_DOVECOT_PUSH_OK,
    /* The message is gone in the daemon; the next sync expunges it. */
    WYREBOX_DOVECOT_PUSH_MESSAGE_GONE,
    /* Only this message failed; later messages are still pushed. */
    WYREBOX_DOVECOT_PUSH_MESSAGE_FAILED,
    /* The change is not representable in WyreBox; nothing was sent. */
    WYREBOX_DOVECOT_PUSH_INVALID,
    /* Every later request would fail the same way. */
    WYREBOX_DOVECOT_PUSH_FAILED,
} WyreboxDovecotPushResult;

static WyreboxDovecotPushResult
wyrebox_dovecot_push_update (struct wyrebox_dovecot_mailbox *wbox,
    WyreboxDovecotMailboxUidMapRow *row,
    WyreboxDaemonFlagKeywordUpdateMode mode, enum mail_flags flags,
    GHashTable *keywords, GError **error)
{
    struct wyrebox_dovecot_storage *storage =
        (struct wyrebox_dovecot_storage *)wbox->box.storage;
    const WyreboxDaemonMailboxSelectResult *select = &wbox->state.select_result;
    g_autoptr (GPtrArray) system_names =
        wyrebox_dovecot_system_flag_names (flags);
    g_autoptr (GPtrArray) keyword_names =
        wyrebox_dovecot_keyword_names (keywords);
    g_auto (WyreboxDaemonFlagKeywordUpdateRequest) request = { 0 };
    const char *const *system_flags =
        (const char *const *)system_names->pdata;
    const char *const *user_keywords =
        (const char *const *)keyword_names->pdata;
    guint64 journal_offset = 0;

    if (system_names->len == 1 && keyword_names->len == 1) {
        return WYREBOX_DOVECOT_PUSH_OK;
    }

    /* Validate here: the client reports a malformed daemon response with
     * the same error code as invalid input. */
    if (!wyrebox_daemon_flag_keyword_update_request_init (&request,
        storage->account_identity, select->mailbox_id, select->uid_validity,
        row->uid, mode, system_flags, user_keywords, error)) {
        e_debug (wbox->box.event, "WyreBox cannot store flags of UID %"
            G_GUINT64_FORMAT ": %s", row->uid, (*error)->message);
        return WYREBOX_DOVECOT_PUSH_INVALID;
    }

    if (!wyrebox_dovecot_daemon_client_update_flags (storage->socket_path,
        storage->account_identity, select->mailbox_id, select->uid_validity,
        row->uid, mode, system_flags, user_keywords, &journal_offset, error)) {
        if (g_error_matches (*error, WYREBOX_DOVECOT_DAEMON_CLIENT_ERROR,
            WYREBOX_DAEMON_ERROR_NOT_FOUND)) {
            return WYREBOX_DOVECOT_PUSH_MESSAGE_GONE;
        }
        if (g_error_matches (*error, WYREBOX_DOVECOT_DAEMON_CLIENT_ERROR,
            WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE)) {
            return WYREBOX_DOVECOT_PUSH_MESSAGE_FAILED;
        }
        return WYREBOX_DOVECOT_PUSH_FAILED;
    }

    e_debug (wbox->box.event, "WyreBox flag update mailbox=%s uid=%"
        G_GUINT64_FORMAT " mode=%s journal_offset=%" G_GUINT64_FORMAT,
        select->mailbox_id, row->uid,
        mode == WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET ? "set" : "clear",
        journal_offset);
    wyrebox_dovecot_row_apply_update (row, mode, system_flags, user_keywords);
    return WYREBOX_DOVECOT_PUSH_OK;
}

static void
wyrebox_dovecot_mailbox_set_flag_update_error (struct wyrebox_dovecot_mailbox
    *wbox, WyreboxDovecotPushResult result, const GError *error)
{
    struct mailbox *box = &wbox->box;

    if (result == WYREBOX_DOVECOT_PUSH_INVALID) {
        mail_storage_set_error (box->storage, MAIL_ERROR_PARAMS,
            t_strdup_printf ("WyreBox cannot store this flag change: %s",
            error->message));
        return;
    }
    if (g_error_matches (error, WYREBOX_DOVECOT_DAEMON_CLIENT_ERROR,
        WYREBOX_DAEMON_ERROR_PERMANENT_FAILURE)) {
        e_warning (box->event, "WyreBox daemon rejected a flag change: %s",
            error->message);
        mail_storage_set_error (box->storage, MAIL_ERROR_PARAMS,
            "WyreBox rejected the flag change");
        return;
    }
    if (g_error_matches (error, WYREBOX_DOVECOT_DAEMON_CLIENT_ERROR,
        WYREBOX_DAEMON_ERROR_PERMISSION_DENIED)) {
        e_error (box->event, "WyreBox daemon refused a flag change: %s",
            error->message);
        mail_storage_set_error (box->storage, MAIL_ERROR_PERM,
            MAIL_ERRSTR_NO_PERMISSION);
        return;
    }
    if (g_error_matches (error, WYREBOX_DOVECOT_DAEMON_CLIENT_ERROR,
        WYREBOX_DAEMON_ERROR_CONFLICT)) {
        wbox->inconsistent = TRUE;
    } else if (wyrebox_dovecot_error_is_unavailable (error)) {
        e_error (box->event, "WyreBox daemon flag update failed: %s",
            error->message);
        mail_storage_set_error (box->storage, MAIL_ERROR_UNAVAILABLE,
            "WyreBox daemon is unavailable");
        return;
    }

    mailbox_set_critical (box, "WyreBox daemon flag update failed: %s",
        error->message);
}

/*
 * Sends the index flag changes committed since the last push to the daemon,
 * one SET and one CLEAR per UID at most. A UID whose update fails is reset
 * to the last flags the daemon accepted.
 */
static int
wyrebox_dovecot_mailbox_push_flag_changes (struct wyrebox_dovecot_mailbox
    *wbox)
{
    struct mailbox *box = &wbox->box;
    struct mail_index_sync_ctx *sync_ctx;
    struct mail_index_view *sync_view;
    struct mail_index_transaction *sync_trans;
    g_autoptr (GHashTable) deltas = NULL;
    g_autoptr (GArray) uids = g_array_new (FALSE, FALSE, sizeof (guint32));
    g_autoptr (GError) first_error = NULL;
    WyreboxDovecotPushResult first_result = WYREBOX_DOVECOT_PUSH_OK;
    GHashTableIter iter;
    gpointer key;
    gboolean stopped = FALSE;
    gboolean collected;
    int ret;

    ret = mail_index_sync_begin (box->index, &sync_ctx, &sync_view,
            &sync_trans, MAIL_INDEX_SYNC_FLAG_REQUIRE_CHANGES |
            MAIL_INDEX_SYNC_FLAG_AVOID_FLAG_UPDATES);
    if (ret <= 0) {
        if (ret < 0) {
            mailbox_set_index_error (box);
        }
        return ret;
    }

    wbox->in_index_sync = TRUE;
    deltas = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
            wyrebox_dovecot_flag_delta_free);
    collected = wyrebox_dovecot_collect_flag_deltas (box, sync_ctx, sync_view,
            deltas);

    g_hash_table_iter_init (&iter, deltas);
    while (g_hash_table_iter_next (&iter, &key, NULL)) {
        guint32 uid = GPOINTER_TO_UINT (key);

        g_array_append_val (uids, uid);
    }
    g_array_sort (uids, wyrebox_dovecot_compare_uid);

    for (guint i = 0; i < uids->len; i++) {
        guint32 uid = g_array_index (uids, guint32, i);
        WyreboxDovecotFlagDelta *delta =
            g_hash_table_lookup (deltas, GUINT_TO_POINTER (uid));
        WyreboxDovecotMailboxUidMapRow *row =
            g_hash_table_lookup (wbox->state.rows_by_uid,
                GUINT_TO_POINTER (uid));
        WyreboxDovecotPushResult result = WYREBOX_DOVECOT_PUSH_OK;
        g_autoptr (GError) error = NULL;

        /* Records that cannot be pushed fail the commit; the reconcile in
         * the next sync_index restores the daemon flags for them. */
        if (row == NULL) {
            mailbox_set_critical (box, "WyreBox has no daemon row for UID %u",
                uid);
            collected = FALSE;
            continue;
        }

        if (!stopped) {
            result = wyrebox_dovecot_push_update (wbox, row,
                    WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_SET,
                    delta->add_flags,
                    delta->add_keywords, &error);
        }
        if (!stopped && result == WYREBOX_DOVECOT_PUSH_OK) {
            result = wyrebox_dovecot_push_update (wbox, row,
                    WYREBOX_DAEMON_FLAG_KEYWORD_UPDATE_MODE_CLEAR,
                    delta->remove_flags, delta->remove_keywords, &error);
        }

        if (stopped || result != WYREBOX_DOVECOT_PUSH_OK) {
            wyrebox_dovecot_index_apply_row_flags (box, sync_trans, delta->seq,
                row);
        }
        if (result == WYREBOX_DOVECOT_PUSH_MESSAGE_GONE) {
            e_warning (box->event, "WyreBox UID %u vanished during a flag "
                "update: %s", uid, error->message);
        } else if (result != WYREBOX_DOVECOT_PUSH_OK) {
            if (first_error == NULL) {
                first_error = g_steal_pointer (&error);
                first_result = result;
            }
            stopped = stopped || result != WYREBOX_DOVECOT_PUSH_MESSAGE_FAILED;
        }
    }

    /* Always commit: a rollback would leave the records pending forever. */
    ret = mail_index_sync_commit (&sync_ctx);
    wbox->in_index_sync = FALSE;
    if (ret < 0) {
        mailbox_set_index_error (box);
        return -1;
    }
    if (first_error != NULL) {
        wyrebox_dovecot_mailbox_set_flag_update_error (wbox, first_result,
            first_error);
        return -1;
    }
    return collected ? 0 : -1;
}

static struct mailbox_sync_context *
wyrebox_dovecot_mailbox_sync_init (struct mailbox *box,
    enum mailbox_sync_flags flags)
{
    int ret;

    ret = wyrebox_dovecot_mailbox_sync_index ((struct wyrebox_dovecot_mailbox
            *)box);
    return index_mailbox_sync_init (box, flags, ret < 0);
}

static struct mailbox *
wyrebox_dovecot_mailbox_alloc (struct mail_storage *storage,
    struct mailbox_list *list, const char *vname, enum mailbox_flags flags)
{
    struct wyrebox_dovecot_mailbox *wbox;
    pool_t pool;

    pool = pool_alloconly_create ("wyrebox mailbox", 1024 * 3);
    wbox = p_new (pool, struct wyrebox_dovecot_mailbox, 1);
    wbox->box = wyrebox_dovecot_mailbox_template;
    wbox->box.pool = pool;
    wbox->box.storage = storage;
    wbox->box.list = list;
    wbox->box.mail_vfuncs = &wyrebox_dovecot_mail_vfuncs;

    index_storage_mailbox_alloc (&wbox->box, vname, flags, MAIL_INDEX_PREFIX);
    return &wbox->box;
}

static bool
wyrebox_dovecot_mailbox_is_readonly (struct mailbox *box)
{
    struct wyrebox_dovecot_mailbox *wbox =
        (struct wyrebox_dovecot_mailbox *)box;

    return index_storage_is_readonly (box) || !wbox->writable;
}

static int
wyrebox_dovecot_mailbox_exists (struct mailbox *box, bool auto_boxes,
    enum mailbox_existence *existence_r)
{
    struct wyrebox_dovecot_storage *storage =
        (struct wyrebox_dovecot_storage *)box->storage;
    g_auto (WyreboxDaemonMailboxSelectResult) status = { 0 };
    g_autoptr (GError) error = NULL;

    (void)auto_boxes;

    if (wyrebox_dovecot_daemon_client_get_mailbox_status (storage->socket_path,
        storage->account_identity, box->vname, &status, &error)) {
        *existence_r = MAILBOX_EXISTENCE_SELECT;
        return 0;
    }

    if (g_error_matches (error, WYREBOX_DOVECOT_DAEMON_CLIENT_ERROR,
        WYREBOX_DAEMON_ERROR_NOT_FOUND)) {
        *existence_r = MAILBOX_EXISTENCE_NONE;
        return 0;
    }

    wyrebox_dovecot_mailbox_set_daemon_error (box, "mailbox STATUS", error);
    return -1;
}

static int
wyrebox_dovecot_mailbox_open (struct mailbox *box)
{
    struct wyrebox_dovecot_mailbox *wbox =
        (struct wyrebox_dovecot_mailbox *)box;
    g_auto (WyreboxDovecotMailboxState) state = { 0 };
    g_autoptr (GError) error = NULL;
    const char *operation = NULL;

    if (!wyrebox_dovecot_mailbox_load_state (wbox, &state, &operation,
        &error)) {
        wyrebox_dovecot_mailbox_set_daemon_error (box, operation, error);
        return -1;
    }

    wyrebox_dovecot_mailbox_install_state (wbox, &state);
    wbox->writable = wbox->state.select_result.kind ==
        WYREBOX_DAEMON_MAILBOX_LIST_ENTRY_ORDINARY;
    return index_storage_mailbox_open (box, FALSE);
}

static void
wyrebox_dovecot_mailbox_close (struct mailbox *box)
{
    struct wyrebox_dovecot_mailbox *wbox =
        (struct wyrebox_dovecot_mailbox *)box;

    wyrebox_dovecot_mailbox_state_clear (&wbox->state);
    wbox->inconsistent = FALSE;
    wbox->writable = FALSE;
    index_storage_mailbox_close (box);
}

static void
wyrebox_dovecot_mailbox_free (struct mailbox *box)
{
    struct wyrebox_dovecot_mailbox *wbox =
        (struct wyrebox_dovecot_mailbox *)box;

    wyrebox_dovecot_mailbox_state_clear (&wbox->state);
    index_storage_mailbox_free (box);
}

static bool
wyrebox_dovecot_mailbox_is_inconsistent (struct mailbox *box)
{
    struct wyrebox_dovecot_mailbox *wbox =
        (struct wyrebox_dovecot_mailbox *)box;

    return index_storage_is_inconsistent (box) || wbox->inconsistent;
}

static int
wyrebox_dovecot_mailbox_not_possible (struct mailbox *box)
{
    mail_storage_set_error (box->storage, MAIL_ERROR_NOTPOSSIBLE,
        "WyreBox does not support this mailbox operation");
    return -1;
}

static int
wyrebox_dovecot_mailbox_create (struct mailbox *box,
    const struct mailbox_update *update, bool directory)
{
    (void)update;
    (void)directory;
    return wyrebox_dovecot_mailbox_not_possible (box);
}

static int
wyrebox_dovecot_mailbox_update (struct mailbox *box,
    const struct mailbox_update *update)
{
    (void)update;
    return wyrebox_dovecot_mailbox_not_possible (box);
}

static int
wyrebox_dovecot_mailbox_rename (struct mailbox *src, struct mailbox *dest)
{
    (void)dest;
    return wyrebox_dovecot_mailbox_not_possible (src);
}

static int
wyrebox_dovecot_mailbox_get_metadata (struct mailbox *box,
    enum mailbox_metadata_items items, struct mailbox_metadata *metadata_r)
{
    if ((items & MAILBOX_METADATA_GUID) != 0) {
        mail_generate_guid_128_hash (box->vname, metadata_r->guid);
        items &= ENUM_NEGATE (MAILBOX_METADATA_GUID);
    }

    if (items != 0) {
        return index_mailbox_get_metadata (box, items, metadata_r);
    }

    return 0;
}

static void
wyrebox_dovecot_mailbox_notify_changes (struct mailbox *box)
{
    (void)box;
}

static struct mail_save_context *
wyrebox_dovecot_save_alloc (struct mailbox_transaction_context *t)
{
    struct mail_save_context *ctx;

    ctx = i_new (struct mail_save_context, 1);
    ctx->transaction = t;
    return ctx;
}

static int
wyrebox_dovecot_save_begin (struct mail_save_context *ctx,
    struct istream *input)
{
    (void)input;
    return wyrebox_dovecot_mailbox_not_possible (ctx->transaction->box);
}

static int
wyrebox_dovecot_save_continue (struct mail_save_context *ctx)
{
    (void)ctx;
    return -1;
}

static int
wyrebox_dovecot_save_finish (struct mail_save_context *ctx)
{
    index_save_context_free (ctx);
    return -1;
}

static void
wyrebox_dovecot_save_cancel (struct mail_save_context *ctx)
{
    index_save_context_free (ctx);
}

static struct mail *
wyrebox_dovecot_mail_alloc (struct mailbox_transaction_context *t,
    enum mail_fetch_field wanted_fields,
    struct mailbox_header_lookup_ctx *wanted_headers)
{
    struct index_mail *mail;
    pool_t pool;

    pool = pool_alloconly_create ("wyrebox mail", 2048);
    mail = p_new (pool, struct index_mail, 1);
    index_mail_init (mail, t, wanted_fields, wanted_headers, pool, NULL);
    return &mail->mail.mail;
}

static int
wyrebox_dovecot_mail_open_stream (struct mail *_mail)
{
    struct index_mail *mail = INDEX_MAIL (_mail);
    struct wyrebox_dovecot_mailbox *wbox =
        (struct wyrebox_dovecot_mailbox *)_mail->box;
    struct wyrebox_dovecot_storage *storage =
        (struct wyrebox_dovecot_storage *)_mail->box->storage;
    const WyreboxDovecotMailboxUidMapRow *row = NULL;
    g_autoptr (GBytes) message = NULL;
    g_autoptr (GError) error = NULL;
    gconstpointer data;
    gsize size = 0;

    if (!mail_stream_access_start (_mail)) {
        return -1;
    }

    if (wbox->state.rows_by_uid != NULL) {
        row = g_hash_table_lookup (wbox->state.rows_by_uid,
                GUINT_TO_POINTER (_mail->uid));
    }
    if (row == NULL) {
        mail_set_expunged (_mail);
        return -1;
    }

    message = wyrebox_dovecot_daemon_client_fetch_message_bytes
            (storage->socket_path, storage->account_identity,
            wbox->state.select_result.mailbox_id,
            wbox->state.select_result.kind,
            wbox->state.select_result.uid_validity, _mail->uid, row->message_id,
            &error);
    if (message == NULL) {
        if (g_error_matches (error, WYREBOX_DOVECOT_DAEMON_CLIENT_ERROR,
            WYREBOX_DAEMON_ERROR_NOT_FOUND)) {
            mail_set_expunged (_mail);
        } else {
            wyrebox_dovecot_mailbox_set_daemon_error (_mail->box, "FETCH",
                error);
        }
        return -1;
    }

    data = g_bytes_get_data (message, &size);
    if (size == 0) {
        mailbox_set_critical (_mail->box,
            "WyreBox daemon returned an empty message for UID %u", _mail->uid);
        return -1;
    }

    mail->data.stream = i_stream_create_copy_from_data (data, size);
    i_stream_set_name (mail->data.stream, t_strdup_printf ("wyrebox UID %u",
        _mail->uid));
    if (mail->mail.v.istream_opened != NULL
        && mail->mail.v.istream_opened (_mail, &mail->data.stream) < 0) {
        index_mail_close_streams (mail);
        return -1;
    }

    return 0;
}

static int
wyrebox_dovecot_mail_get_stream (struct mail *_mail, bool get_body,
    struct message_size *hdr_size, struct message_size *body_size,
    struct istream **stream_r)
{
    struct index_mail *mail = INDEX_MAIL (_mail);

    (void)get_body;

    if (mail->data.stream == NULL && wyrebox_dovecot_mail_open_stream (_mail)
        < 0) {
        return -1;
    }

    return index_mail_init_stream (mail, hdr_size, body_size, stream_r);
}

static int
wyrebox_dovecot_mail_get_physical_size (struct mail *_mail, uoff_t *size_r)
{
    struct index_mail *mail = INDEX_MAIL (_mail);
    struct message_size hdr_size, body_size;
    struct istream *input;

    if (index_mail_get_physical_size (_mail, size_r) == 0) {
        return 0;
    }

    if (mail_get_stream (_mail, &hdr_size, &body_size, &input) < 0) {
        return -1;
    }

    i_assert (mail->data.physical_size != UOFF_T_MAX);
    *size_r = mail->data.physical_size;
    return 0;
}

static int
wyrebox_dovecot_mail_get_received_date (struct mail *_mail, time_t *date_r)
{
    int tz;

    return index_mail_get_date (_mail, date_r, &tz);
}

static int
wyrebox_dovecot_mail_get_save_date (struct mail *_mail, time_t *date_r)
{
    return wyrebox_dovecot_mail_get_received_date (_mail, date_r);
}

static struct wyrebox_dovecot_transaction *
wyrebox_dovecot_mail_transaction (struct mail *_mail)
{
    return (struct wyrebox_dovecot_transaction *)_mail->transaction;
}

static void
wyrebox_dovecot_mail_update_flags (struct mail *_mail,
    enum modify_type modify_type, enum mail_flags flags)
{
    wyrebox_dovecot_mail_transaction (_mail)->flags_changed = TRUE;
    index_mail_update_flags (_mail, modify_type, flags);
}

static void
wyrebox_dovecot_mail_update_keywords (struct mail *_mail,
    enum modify_type modify_type, struct mail_keywords *keywords)
{
    wyrebox_dovecot_mail_transaction (_mail)->flags_changed = TRUE;
    index_mail_update_keywords (_mail, modify_type, keywords);
}

/* The refusal surfaces at commit as MAIL_ERROR_PERM. */
static void
wyrebox_dovecot_mail_expunge (struct mail *_mail)
{
    wyrebox_dovecot_mail_transaction (_mail)->expunge_refused = TRUE;
}

static struct mailbox_transaction_context *
wyrebox_dovecot_transaction_begin (struct mailbox *box,
    enum mailbox_transaction_flags flags, const char *reason)
{
    struct wyrebox_dovecot_transaction *wt;

    wt = i_new (struct wyrebox_dovecot_transaction, 1);
    index_transaction_init (&wt->t, box, flags, reason);
    return &wt->t;
}

static int
wyrebox_dovecot_transaction_commit (struct mailbox_transaction_context *t,
    struct mail_transaction_commit_changes *changes_r)
{
    struct wyrebox_dovecot_transaction *wt =
        (struct wyrebox_dovecot_transaction *)t;
    struct wyrebox_dovecot_mailbox *wbox =
        (struct wyrebox_dovecot_mailbox *)t->box;
    struct mailbox *box = t->box;

    if (wt->expunge_refused) {
        index_transaction_rollback (t);
        mail_storage_set_error (box->storage, MAIL_ERROR_PERM,
            "WyreBox does not support EXPUNGE yet");
        return -1;
    }
    if (wt->flags_changed && mailbox_is_readonly (box)) {
        index_transaction_rollback (t);
        mail_storage_set_error (box->storage, MAIL_ERROR_NOTPOSSIBLE,
            "WyreBox mailbox is read-only");
        return -1;
    }
    if (index_transaction_commit (t, changes_r) < 0) {
        return -1;
    }
    if ((changes_r->changes_mask & (MAIL_INDEX_TRANSACTION_CHANGE_FLAGS |
        MAIL_INDEX_TRANSACTION_CHANGE_KEYWORDS)) == 0) {
        return 0;
    }
    if (wbox->in_index_sync) {
        /* The pending records are replaced by the next sync's reconcile. */
        mailbox_set_critical (box,
            "WyreBox flag change committed during an index sync");
        return -1;
    }
    return wyrebox_dovecot_mailbox_push_flag_changes (wbox);
}

static struct mail_vfuncs wyrebox_dovecot_mail_vfuncs = {
    .close = index_mail_close,
    .free = index_mail_free,
    .set_seq = index_mail_set_seq,
    .set_uid = index_mail_set_uid,
    .set_uid_cache_updates = index_mail_set_uid_cache_updates,
    .prefetch = index_mail_prefetch,
    .precache = index_mail_precache,
    .add_temp_wanted_fields = index_mail_add_temp_wanted_fields,
    .get_flags = index_mail_get_flags,
    .get_keywords = index_mail_get_keywords,
    .get_keyword_indexes = index_mail_get_keyword_indexes,
    .get_modseq = index_mail_get_modseq,
    .get_pvt_modseq = index_mail_get_pvt_modseq,
    .get_parts = index_mail_get_parts,
    .get_date = index_mail_get_date,
    .get_received_date = wyrebox_dovecot_mail_get_received_date,
    .get_save_date = wyrebox_dovecot_mail_get_save_date,
    .get_virtual_size = index_mail_get_virtual_size,
    .get_physical_size = wyrebox_dovecot_mail_get_physical_size,
    .get_first_header = index_mail_get_first_header,
    .get_headers = index_mail_get_headers,
    .get_header_stream = index_mail_get_header_stream,
    .get_stream = wyrebox_dovecot_mail_get_stream,
    .get_binary_stream = index_mail_get_binary_stream,
    .get_special = index_mail_get_special,
    .get_backend_mail = index_mail_get_backend_mail,
    .update_flags = wyrebox_dovecot_mail_update_flags,
    .update_keywords = wyrebox_dovecot_mail_update_keywords,
    .update_modseq = index_mail_update_modseq,
    .update_pvt_modseq = index_mail_update_pvt_modseq,
    .expunge = wyrebox_dovecot_mail_expunge,
    .set_cache_corrupted = index_mail_set_cache_corrupted,
    .istream_opened = index_mail_opened,
};

static struct mailbox wyrebox_dovecot_mailbox_template = {
    .v = {
        .is_readonly = wyrebox_dovecot_mailbox_is_readonly,
        .enable = index_storage_mailbox_enable,
        .exists = wyrebox_dovecot_mailbox_exists,
        .open = wyrebox_dovecot_mailbox_open,
        .close = wyrebox_dovecot_mailbox_close,
        .free = wyrebox_dovecot_mailbox_free,
        .create_box = wyrebox_dovecot_mailbox_create,
        .update_box = wyrebox_dovecot_mailbox_update,
        .delete_box = wyrebox_dovecot_mailbox_not_possible,
        .rename_box = wyrebox_dovecot_mailbox_rename,
        .get_status = index_storage_get_status,
        .get_metadata = wyrebox_dovecot_mailbox_get_metadata,
        .set_subscribed = index_storage_set_subscribed,
        .attribute_set = index_storage_attribute_set,
        .attribute_get = index_storage_attribute_get,
        .attribute_iter_init = index_storage_attribute_iter_init,
        .attribute_iter_next = index_storage_attribute_iter_next,
        .attribute_iter_deinit = index_storage_attribute_iter_deinit,
        .list_index_has_changed = index_storage_list_index_has_changed,
        .list_index_update_sync = index_storage_list_index_update_sync,
        .sync_init = wyrebox_dovecot_mailbox_sync_init,
        .sync_next = index_mailbox_sync_next,
        .sync_deinit = index_mailbox_sync_deinit,
        .notify_changes = wyrebox_dovecot_mailbox_notify_changes,
        .transaction_begin = wyrebox_dovecot_transaction_begin,
        .transaction_commit = wyrebox_dovecot_transaction_commit,
        .transaction_rollback = index_transaction_rollback,
        .mail_alloc = wyrebox_dovecot_mail_alloc,
        .search_init = index_storage_search_init,
        .search_deinit = index_storage_search_deinit,
        .search_next_nonblock = index_storage_search_next_nonblock,
        .search_next_update_seq = index_storage_search_next_update_seq,
        .search_next_match_mail = index_storage_search_next_match_mail,
        .save_alloc = wyrebox_dovecot_save_alloc,
        .save_begin = wyrebox_dovecot_save_begin,
        .save_continue = wyrebox_dovecot_save_continue,
        .save_finish = wyrebox_dovecot_save_finish,
        .save_cancel = wyrebox_dovecot_save_cancel,
        .copy = mail_storage_copy,
        .is_inconsistent = wyrebox_dovecot_mailbox_is_inconsistent,
    },
};

static struct mail_storage *
wyrebox_dovecot_storage_alloc (void)
{
    struct wyrebox_dovecot_storage *storage;
    pool_t pool;

    pool = pool_alloconly_create ("wyrebox storage", 512 + 256);
    storage = p_new (pool, struct wyrebox_dovecot_storage, 1);
    storage->storage = wyrebox_mail_storage_class;
    storage->storage.pool = pool;
    return &storage->storage;
}

static int
wyrebox_dovecot_storage_create (struct mail_storage *storage,
    struct mail_namespace *ns, const char **error_r)
{
    struct wyrebox_dovecot_storage *wstorage =
        (struct wyrebox_dovecot_storage *)storage;

    if (ns == NULL || ns->user == NULL || ns->user->username == NULL ||
        ns->user->username[0] == '\0') {
        *error_r = "Dovecot namespace user identity is unavailable";
        return -1;
    }

    if (ns->list == NULL
        || strcmp (ns->list->name, WYREBOX_DOVECOT_LIST_NAME) != 0) {
        *error_r = "mail_driver = wyrebox requires mailbox_list_layout = "
            WYREBOX_DOVECOT_LIST_NAME;
        return -1;
    }

    /* Attachment flags are $-keywords the daemon cannot store. */
    if (storage->set->parsed_mail_attachment_detection_add_flags) {
        *error_r = "mail_driver = wyrebox requires "
            "mail_attachment_detection_options without add-flags";
        return -1;
    }

    wstorage->socket_path = p_strdup (storage->pool,
            wyrebox_dovecot_socket_path ());
    wstorage->account_identity = p_strdup (storage->pool, ns->user->username);
    return 0;
}

static void
wyrebox_dovecot_storage_destroy (struct mail_storage *storage)
{
    index_storage_destroy (storage);
}

static struct mail_storage wyrebox_mail_storage_class = {
    .name = "wyrebox",
    .class_flags = MAIL_STORAGE_CLASS_FLAG_NO_ROOT,
    .v = {
        .alloc = wyrebox_dovecot_storage_alloc,
        .create = wyrebox_dovecot_storage_create,
        .destroy = wyrebox_dovecot_storage_destroy,
        .mailbox_alloc = wyrebox_dovecot_mailbox_alloc,
    },
};

static struct mailbox_list wyrebox_mailbox_list_class = {
    .name = WYREBOX_DOVECOT_LIST_NAME,
    .props = MAILBOX_LIST_PROP_NO_ROOT | MAILBOX_LIST_PROP_NO_LIST_INDEX,
    .mailbox_name_max_length = MAILBOX_LIST_NAME_MAX_LENGTH,
    .v = {
        .alloc = wyrebox_dovecot_list_alloc,
        .deinit = wyrebox_dovecot_list_deinit,
        .get_storage = mailbox_list_default_get_storage,
        .get_hierarchy_sep = wyrebox_dovecot_list_get_hierarchy_sep,
        .get_vname = mailbox_list_default_get_vname,
        .get_storage_name = mailbox_list_default_get_storage_name,
        .get_path = wyrebox_dovecot_list_get_path,
        .get_temp_prefix = wyrebox_dovecot_list_get_temp_prefix,
        .iter_init = wyrebox_dovecot_list_iter_init,
        .iter_next = wyrebox_dovecot_list_iter_next,
        .iter_deinit = wyrebox_dovecot_list_iter_deinit,
        .get_mailbox_flags = wyrebox_dovecot_list_get_mailbox_flags,
        .subscriptions_refresh = wyrebox_dovecot_list_subscriptions_refresh,
        .set_subscribed = wyrebox_dovecot_list_set_subscribed,
        .delete_mailbox = wyrebox_dovecot_list_delete_mailbox,
        .delete_dir = wyrebox_dovecot_list_delete_mailbox,
        .delete_symlink = wyrebox_dovecot_list_delete_mailbox,
        .rename_mailbox = wyrebox_dovecot_list_rename_mailbox,
    },
};

#define WYREBOX_DOVECOT_PLUGIN_EXPORT __attribute__ ((visibility ("default")))

WYREBOX_DOVECOT_PLUGIN_EXPORT const char *wyrebox_plugin_version =
    DOVECOT_ABI_VERSION;

WYREBOX_DOVECOT_PLUGIN_EXPORT void
wyrebox_plugin_init (struct module *module)
{
    (void)module;
    mailbox_list_register (&wyrebox_mailbox_list_class);
    mail_storage_class_register (&wyrebox_mail_storage_class);
}

WYREBOX_DOVECOT_PLUGIN_EXPORT void
wyrebox_plugin_deinit (void)
{
    mail_storage_class_unregister (&wyrebox_mail_storage_class);
    mailbox_list_unregister (&wyrebox_mailbox_list_class);
}
