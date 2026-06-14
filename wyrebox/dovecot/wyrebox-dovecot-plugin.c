#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#include "lib.h"
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

struct wyrebox_dovecot_storage
{
  struct mail_storage storage;
  char *socket_path;
  char *account_identity;
};

struct wyrebox_dovecot_mailbox
{
  struct mailbox mailbox;
  WyreboxDaemonMailboxSelectResult select_result;
  int select_result_valid;
  WyreboxDovecotMailboxUidMapSnapshot uid_map_snapshot;
};

typedef struct
{
  struct mailbox_list_vfuncs previous_vfuncs;
  struct mailbox_list_vfuncs *previous_vlast;
  char *socket_path;
  char *account_identity;
} WyreboxDovecotMailboxListHookContext;

typedef struct
{
  struct mailbox_list_iterate_context ctx;
  struct mailbox_info *entries;
  guint n_entries;
  guint next_entry;
} WyreboxDovecotMailboxListIterContext;

typedef gboolean (*WyreboxDovecotMailboxListPublishFunc) (struct mailbox_list
    * list, const char *name, char hierarchy_delimiter, gboolean selectable,
    enum mailbox_list_child_state child_state, const char *special_use,
    gpointer user_data);

typedef struct
{
  const char *name;
  char hierarchy_delimiter;
  gboolean selectable;
  enum mailbox_list_child_state child_state;
  const char *special_use;
} WyreboxDovecotMailboxListMappedEntry;

static struct mail_storage wyrebox_mail_storage_class;
static GHashTable *wyrebox_dovecot_mailbox_list_hook_contexts;

extern const char *wyrebox_dovecot_test_daemon_socket_path
    __attribute__((weak));

static const char *
wyrebox_dovecot_socket_path (void)
{
  if (&wyrebox_dovecot_test_daemon_socket_path != NULL
      && wyrebox_dovecot_test_daemon_socket_path != NULL
      && wyrebox_dovecot_test_daemon_socket_path[0] != '\0') {
    return wyrebox_dovecot_test_daemon_socket_path;
  }

  return "/run/wyrebox/wyrebox.sock";
}

static void
wyrebox_dovecot_mailbox_list_hook_context_free (gpointer data)
{
  WyreboxDovecotMailboxListHookContext *context = data;

  if (context == NULL) {
    return;
  }

  g_free (context->socket_path);
  g_free (context->account_identity);
  g_free (context);
}

static void
    wyrebox_dovecot_mailbox_list_iter_context_free
    (WyreboxDovecotMailboxListIterContext * context)
{
  if (context == NULL) {
    return;
  }

  for (guint i = 0; i < context->n_entries; i++) {
    g_free ((char *) context->entries[i].vname);
    g_free ((char *) context->entries[i].special_use);
  }
  g_free (context->entries);
  g_free (context);
}

static WyreboxDovecotMailboxListHookContext *
wyrebox_dovecot_mailbox_list_hook_context_lookup (struct mailbox_list *list)
{
  if (wyrebox_dovecot_mailbox_list_hook_contexts == NULL) {
    return NULL;
  }

  return g_hash_table_lookup (wyrebox_dovecot_mailbox_list_hook_contexts, list);
}

static gboolean
wyrebox_dovecot_mailbox_list_restore_hook (struct mailbox_list *list,
    struct mailbox_list_vfuncs *previous_vfuncs_r)
{
  WyreboxDovecotMailboxListHookContext *context;

  context = wyrebox_dovecot_mailbox_list_hook_context_lookup (list);
  if (context == NULL) {
    return FALSE;
  }

  if (previous_vfuncs_r != NULL) {
    *previous_vfuncs_r = context->previous_vfuncs;
  }

  list->v = context->previous_vfuncs;
  list->vlast = context->previous_vlast;
  g_hash_table_remove (wyrebox_dovecot_mailbox_list_hook_contexts, list);
  return TRUE;
}

static enum mailbox_info_flags
    wyrebox_dovecot_mailbox_list_flags_from_entry
    (const WyreboxDaemonMailboxListEntry * entry)
{
  enum mailbox_info_flags flags = 0;

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
    wyrebox_dovecot_mailbox_list_iter_context_append_entry
    (WyreboxDovecotMailboxListIterContext * context,
    const WyreboxDaemonMailboxListEntry * entry, GError ** error)
{
  struct mailbox_info *info;

  if (entry == NULL || entry->mailbox_name == NULL
      || entry->mailbox_name[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_DATA,
        "Dovecot mailbox LIST iterator received invalid mailbox name");
    return FALSE;
  }

  info = &context->entries[context->n_entries];
  info->vname = g_strdup (entry->mailbox_name);
  if (info->vname == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "Dovecot mailbox LIST iterator failed to copy mailbox name");
    return FALSE;
  }

  if (entry->special_use != NULL) {
    info->special_use = g_strdup (entry->special_use);
    if (info->special_use == NULL) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_FAILED,
          "Dovecot mailbox LIST iterator failed to copy special-use flag");
      g_free ((char *) info->vname);
      info->vname = NULL;
      return FALSE;
    }
  }

  info->flags = wyrebox_dovecot_mailbox_list_flags_from_entry (entry);
  info->ns = NULL;
  context->n_entries++;
  return TRUE;
}

static void
wyrebox_dovecot_mailbox_list_restore_all_hooks (void)
{
  while (wyrebox_dovecot_mailbox_list_hook_contexts != NULL
      && g_hash_table_size (wyrebox_dovecot_mailbox_list_hook_contexts) > 0) {
    GHashTableIter iter;
    gpointer list = NULL;

    g_hash_table_iter_init (&iter, wyrebox_dovecot_mailbox_list_hook_contexts);
    if (!g_hash_table_iter_next (&iter, &list, NULL)) {
      break;
    }

    wyrebox_dovecot_mailbox_list_restore_hook (list, NULL);
  }
}

static struct mailbox_list_iterate_context *
wyrebox_dovecot_mailbox_list_iter_init (struct mailbox_list *list,
    const char *const *patterns, enum mailbox_list_iter_flags flags)
{
  WyreboxDovecotMailboxListHookContext *hook_context;
  WyreboxDovecotMailboxListIterContext *context;
  g_auto (WyreboxDaemonMailboxListResult) result = { 0 };
  g_autoptr (GError) error = NULL;
  guint n_entries;

  (void) patterns;

  hook_context = wyrebox_dovecot_mailbox_list_hook_context_lookup (list);
  if (hook_context == NULL) {
    return NULL;
  }

  context = g_new0 (WyreboxDovecotMailboxListIterContext, 1);
  context->ctx.list = list;
  context->ctx.flags = flags;

  if (!wyrebox_dovecot_daemon_client_list_mailboxes (hook_context->socket_path,
          hook_context->account_identity, "", &result, &error)) {
    context->ctx.failed = TRUE;
    return &context->ctx;
  }

  n_entries = wyrebox_daemon_mailbox_list_result_get_n_entries (&result);
  context->entries = g_new0 (struct mailbox_info, n_entries);

  for (guint i = 0; i < n_entries; i++) {
    const WyreboxDaemonMailboxListEntry *entry =
        wyrebox_daemon_mailbox_list_result_get_entry (&result, i);

    if (!wyrebox_dovecot_mailbox_list_iter_context_append_entry (context,
            entry, &error)) {
      context->ctx.failed = TRUE;
      break;
    }
  }

  return &context->ctx;
}

static const struct mailbox_info *
wyrebox_dovecot_mailbox_list_iter_next (struct
    mailbox_list_iterate_context *ctx)
{
  WyreboxDovecotMailboxListIterContext *context;

  if (ctx == NULL) {
    return NULL;
  }

  context = (WyreboxDovecotMailboxListIterContext *) ctx;
  if (context->next_entry >= context->n_entries) {
    return NULL;
  }

  return &context->entries[context->next_entry++];
}

static int
wyrebox_dovecot_mailbox_list_iter_deinit (struct
    mailbox_list_iterate_context *ctx)
{
  WyreboxDovecotMailboxListIterContext *context;
  gboolean failed;

  if (ctx == NULL) {
    return 0;
  }

  context = (WyreboxDovecotMailboxListIterContext *) ctx;
  failed = ctx->failed;
  wyrebox_dovecot_mailbox_list_iter_context_free (context);
  return failed ? -1 : 0;
}

static void
wyrebox_dovecot_mailbox_list_deinit (struct mailbox_list *list)
{
  struct mailbox_list_vfuncs previous_vfuncs = { 0 };

  if (!wyrebox_dovecot_mailbox_list_restore_hook (list, &previous_vfuncs)) {
    return;
  }

  if (previous_vfuncs.deinit != NULL) {
    previous_vfuncs.deinit (list);
  }
}

static gboolean
wyrebox_dovecot_map_mailbox_list_child_state (WyreboxDaemonMailboxListChildState
    in, enum mailbox_list_child_state *out, GError **error)
{
  switch (in) {
    case WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_UNKNOWN:
      *out = MAILBOX_LIST_CHILD_STATE_UNKNOWN;
      return TRUE;
    case WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN:
      *out = MAILBOX_LIST_CHILD_STATE_HAS_CHILDREN;
      return TRUE;
    case WYREBOX_DAEMON_MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN:
      *out = MAILBOX_LIST_CHILD_STATE_HAS_NO_CHILDREN;
      return TRUE;
    default:
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "Dovecot mailbox LIST publication received unsupported child state");
      return FALSE;
  }
}

gboolean
wyrebox_dovecot_publish_mailbox_list_result (struct mailbox_list *list,
    const WyreboxDaemonMailboxListResult *result,
    WyreboxDovecotMailboxListPublishFunc publisher, gpointer publisher_data,
    GError **error)
{
  g_autofree WyreboxDovecotMailboxListMappedEntry *mapped_entries = NULL;
  guint n_entries;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (list == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot mailbox LIST publication requires mailbox_list");
    return FALSE;
  }

  if (result == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot mailbox LIST publication requires daemon result");
    return FALSE;
  }

  if (publisher == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot mailbox LIST publication requires publisher callback");
    return FALSE;
  }

  if (result->entries == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "Dovecot mailbox LIST publication requires initialized daemon result");
    return FALSE;
  }

  n_entries = wyrebox_daemon_mailbox_list_result_get_n_entries (result);
  mapped_entries = g_new0 (WyreboxDovecotMailboxListMappedEntry, n_entries);

  for (guint i = 0; i < n_entries; i++) {
    const WyreboxDaemonMailboxListEntry *entry =
        wyrebox_daemon_mailbox_list_result_get_entry (result, i);
    WyreboxDovecotMailboxListMappedEntry *mapped_entry = &mapped_entries[i];

    if (entry == NULL || entry->mailbox_name == NULL ||
        entry->mailbox_name[0] == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "Dovecot mailbox LIST publication received invalid mailbox name");
      return FALSE;
    }

    if (entry->hierarchy_delimiter == NULL
        || entry->hierarchy_delimiter[0] == '\0'
        || entry->hierarchy_delimiter[1] != '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_DATA,
          "Dovecot mailbox LIST publication requires one-character delimiter");
      return FALSE;
    }

    if (!wyrebox_dovecot_map_mailbox_list_child_state (entry->child_state,
            &mapped_entry->child_state, error))
      return FALSE;

    mapped_entry->name = entry->mailbox_name;
    mapped_entry->hierarchy_delimiter = entry->hierarchy_delimiter[0];
    mapped_entry->selectable = entry->is_selectable;
    mapped_entry->special_use = entry->special_use;
  }

  for (guint i = 0; i < n_entries; i++) {
    const WyreboxDovecotMailboxListMappedEntry *entry = &mapped_entries[i];

    if (!publisher (list, entry->name, entry->hierarchy_delimiter,
            entry->selectable, entry->child_state, entry->special_use,
            publisher_data)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_FAILED,
          "Dovecot mailbox LIST publication failed for mailbox '%s'",
          entry->name);
      return FALSE;
    }
  }

  return TRUE;
}

static char *
wyrebox_dovecot_strdup (const char *str)
{
  size_t size;
  char *copy;

  size = strlen (str) + 1;
  copy = malloc (size);
  if (copy == NULL) {
    return NULL;
  }

  memcpy (copy, str, size);
  return copy;
}

static const char *
wyrebox_dovecot_mailbox_error_message (const GError *error)
{
  return error != NULL && error->message != NULL
      ? error->message : "WyreBox mailbox open failed";
}

static void
wyrebox_dovecot_mailbox_set_opened (struct mailbox *box, gboolean opened)
{
  box->opened = opened;
}

static void
wyrebox_dovecot_mailbox_clear_cache (struct wyrebox_dovecot_mailbox *wbox)
{
  wyrebox_daemon_mailbox_select_result_clear (&wbox->select_result);
  wyrebox_dovecot_mailbox_uid_map_snapshot_clear (&wbox->uid_map_snapshot);
  wbox->select_result_valid = 0;
}

static int
wyrebox_dovecot_mailbox_enable (struct mailbox *box,
    enum mailbox_feature features)
{
  box->enabled_features |= features;
  return 0;
}

static void
wyrebox_dovecot_mailbox_close (struct mailbox *box)
{
  struct wyrebox_dovecot_mailbox *wbox = (struct wyrebox_dovecot_mailbox *) box;

  wyrebox_dovecot_mailbox_set_opened (box, FALSE);
  wyrebox_dovecot_mailbox_clear_cache (wbox);
}

static struct mailbox_sync_context *
wyrebox_dovecot_mailbox_sync_init (struct mailbox *box,
    enum mailbox_sync_flags flags)
{
  struct mailbox_sync_context *ctx;

  ctx = malloc (sizeof (*ctx));
  if (ctx == NULL) {
    return NULL;
  }

  ctx->box = box;
  ctx->flags = flags;
  ctx->open_failed = FALSE;
  return ctx;
}

static bool
wyrebox_dovecot_mailbox_sync_next (struct mailbox_sync_context *ctx,
    struct mailbox_sync_rec *sync_rec_r)
{
  (void) ctx;
  (void) sync_rec_r;
  return FALSE;
}

static int
wyrebox_dovecot_mailbox_sync_deinit (struct mailbox_sync_context *ctx,
    struct mailbox_sync_status *status_r)
{
  if (status_r != NULL) {
    memset (status_r, 0, sizeof (*status_r));
  }
  free (ctx);
  return 0;
}

static gboolean
wyrebox_dovecot_mailbox_refresh_select_result (struct mailbox *box,
    GError **error)
{
  struct wyrebox_dovecot_mailbox *wbox = (struct wyrebox_dovecot_mailbox *) box;
  struct wyrebox_dovecot_storage *storage =
      (struct wyrebox_dovecot_storage *) box->storage;
  g_auto (WyreboxDaemonMailboxSelectResult) select_result = { 0 };
  g_auto (WyreboxDovecotMailboxUidMapSnapshot) uid_map_snapshot = { 0 };

  wyrebox_dovecot_mailbox_clear_cache (wbox);

  if (!wyrebox_dovecot_daemon_client_select_mailbox (storage->socket_path,
          storage->account_identity, box->vname, &select_result, error)) {
    return FALSE;
  }

  if (!wyrebox_daemon_mailbox_select_result_init (&wbox->select_result,
          select_result.kind, select_result.mailbox_id,
          select_result.mailbox_name, select_result.uid_validity,
          select_result.uid_next, select_result.message_count, error)) {
    wyrebox_dovecot_mailbox_clear_cache (wbox);
    return FALSE;
  }

  if (!wyrebox_dovecot_daemon_client_load_uid_map (storage->socket_path,
          storage->account_identity, select_result.mailbox_id,
          select_result.kind,
          select_result.uid_validity, &uid_map_snapshot, error)) {
    wyrebox_dovecot_mailbox_clear_cache (wbox);
    return FALSE;
  }

  wbox->uid_map_snapshot.rows = g_steal_pointer (&uid_map_snapshot.rows);
  wbox->select_result_valid = 1;
  return TRUE;
}

static void
wyrebox_dovecot_mailbox_free (struct mailbox *box)
{
  struct wyrebox_dovecot_mailbox *wbox = (struct wyrebox_dovecot_mailbox *) box;

  wyrebox_dovecot_mailbox_clear_cache (wbox);
  event_unref (&box->event);
}

static int
wyrebox_dovecot_mailbox_open (struct mailbox *box)
{
  g_autoptr (GError) error = NULL;

  wyrebox_dovecot_mailbox_set_opened (box, FALSE);
  if (!wyrebox_dovecot_mailbox_refresh_select_result (box, &error)) {
    mail_storage_set_error (box->storage, MAIL_ERROR_NOTPOSSIBLE,
        wyrebox_dovecot_mailbox_error_message (error));
    return -1;
  }

  wyrebox_dovecot_mailbox_set_opened (box, TRUE);
  return 0;
}

static int
wyrebox_dovecot_mailbox_get_status (struct mailbox *box,
    enum mailbox_status_items items, struct mailbox_status *status_r)
{
  struct wyrebox_dovecot_mailbox *wbox = (struct wyrebox_dovecot_mailbox *) box;
  g_autoptr (GError) error = NULL;

  if (!wbox->select_result_valid
      && !wyrebox_dovecot_mailbox_refresh_select_result (box, &error)) {
    mail_storage_set_error (box->storage, MAIL_ERROR_NOTPOSSIBLE,
        wyrebox_dovecot_mailbox_error_message (error));
    return -1;
  }

  (void) items;

  status_r->messages = wbox->select_result.message_count;
  status_r->uidvalidity = wbox->select_result.uid_validity;
  status_r->uidnext = wbox->select_result.uid_next;
  return 0;
}

static struct mailbox *
wyrebox_dovecot_mailbox_alloc (struct mail_storage *storage,
    struct mailbox_list *list, const char *vname, enum mailbox_flags flags)
{
  pool_t pool;
  struct wyrebox_dovecot_mailbox *wbox;
  const char *name;

  pool = pool_alloconly_create ("wyrebox mailbox",
      sizeof (struct wyrebox_dovecot_mailbox));
  wbox = p_new (pool, struct wyrebox_dovecot_mailbox, 1);

  wbox->mailbox.pool = pool;
  wbox->mailbox.storage = storage;
  wbox->mailbox.list = list;
  wbox->mailbox.vname = p_strdup (pool, vname);
  name = mailbox_list_get_storage_name (list, vname);
  if (name == NULL) {
    name = vname;
  }
  wbox->mailbox.name = p_strdup (pool, name);
  wbox->mailbox.event = event_create (storage->event);
  wbox->mailbox.mail_vfuncs = NULL;
  wbox->mailbox.vlast = NULL;
  wbox->mailbox.v.open = wyrebox_dovecot_mailbox_open;
  wbox->mailbox.v.get_status = wyrebox_dovecot_mailbox_get_status;
  wbox->mailbox.v.free = wyrebox_dovecot_mailbox_free;
  wbox->mailbox.v.enable = wyrebox_dovecot_mailbox_enable;
  wbox->mailbox.v.close = wyrebox_dovecot_mailbox_close;
  wbox->mailbox.v.sync_init = wyrebox_dovecot_mailbox_sync_init;
  wbox->mailbox.v.sync_next = wyrebox_dovecot_mailbox_sync_next;
  wbox->mailbox.v.sync_deinit = wyrebox_dovecot_mailbox_sync_deinit;
  wbox->mailbox.opened = FALSE;
  wbox->mailbox.enabled_features = 0;
  wyrebox_daemon_mailbox_select_result_clear (&wbox->select_result);
  wbox->select_result_valid = 0;
  p_array_init (&wbox->mailbox.search_results, pool, 16);
  p_array_init (&wbox->mailbox.module_contexts, pool, 5);

  (void) flags;
  return &wbox->mailbox;
}

static struct mail_storage *
wyrebox_dovecot_storage_alloc (void)
{
  pool_t pool;
  struct wyrebox_dovecot_storage *storage;

  pool = pool_alloconly_create ("wyrebox storage",
      sizeof (struct wyrebox_dovecot_storage));
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
      (struct wyrebox_dovecot_storage *) storage;
  const char *account_identity;

  if (ns == NULL || ns->user == NULL || ns->user->username == NULL ||
      ns->user->username[0] == '\0') {
    if (error_r != NULL) {
      *error_r = "Dovecot namespace user identity is unavailable";
    }
    return -1;
  }

  account_identity = ns->user->username;

  wstorage->socket_path =
      wyrebox_dovecot_strdup (wyrebox_dovecot_socket_path ());
  wstorage->account_identity = wyrebox_dovecot_strdup (account_identity);
  if (wstorage->socket_path == NULL || wstorage->account_identity == NULL) {
    free (wstorage->socket_path);
    free (wstorage->account_identity);
    wstorage->socket_path = NULL;
    wstorage->account_identity = NULL;
    if (error_r != NULL) {
      *error_r = "Failed to allocate WyreBox Dovecot storage state";
    }
    return -1;
  }

  return 0;
}

static void
wyrebox_dovecot_storage_destroy (struct mail_storage *storage)
{
  struct wyrebox_dovecot_storage *wstorage =
      (struct wyrebox_dovecot_storage *) storage;

  free (wstorage->socket_path);
  free (wstorage->account_identity);
  wstorage->socket_path = NULL;
  wstorage->account_identity = NULL;
}

static void
wyrebox_dovecot_storage_add_list (struct mail_storage *storage,
    struct mailbox_list *list)
{
  WyreboxDovecotMailboxListHookContext *context;
  struct wyrebox_dovecot_storage *wstorage =
      (struct wyrebox_dovecot_storage *) storage;

  if (storage == NULL || list == NULL) {
    return;
  }

  if (wyrebox_dovecot_mailbox_list_hook_context_lookup (list) != NULL) {
    return;
  }

  context = g_new0 (WyreboxDovecotMailboxListHookContext, 1);
  context->previous_vfuncs = list->v;
  context->previous_vlast = list->vlast;
  context->socket_path = g_strdup (wstorage->socket_path);
  context->account_identity = g_strdup (wstorage->account_identity);
  if (context->socket_path == NULL || context->account_identity == NULL) {
    wyrebox_dovecot_mailbox_list_hook_context_free (context);
    return;
  }

  if (wyrebox_dovecot_mailbox_list_hook_contexts == NULL) {
    wyrebox_dovecot_mailbox_list_hook_contexts =
        g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
        wyrebox_dovecot_mailbox_list_hook_context_free);
  }

  g_hash_table_insert (wyrebox_dovecot_mailbox_list_hook_contexts, list,
      context);

  list->vlast = &context->previous_vfuncs;
  list->v.deinit = wyrebox_dovecot_mailbox_list_deinit;
  list->v.iter_init = wyrebox_dovecot_mailbox_list_iter_init;
  list->v.iter_next = wyrebox_dovecot_mailbox_list_iter_next;
  list->v.iter_deinit = wyrebox_dovecot_mailbox_list_iter_deinit;
}

static struct mail_storage wyrebox_mail_storage_class = {
  .name = "wyrebox",
  .class_flags = 0,
  .v = {
        .alloc = wyrebox_dovecot_storage_alloc,
        .create = wyrebox_dovecot_storage_create,
        .destroy = wyrebox_dovecot_storage_destroy,
        .add_list = wyrebox_dovecot_storage_add_list,
        .mailbox_alloc = wyrebox_dovecot_mailbox_alloc,
      },
};

#define WYREBOX_DOVECOT_PLUGIN_EXPORT __attribute__ ((visibility ("default")))

WYREBOX_DOVECOT_PLUGIN_EXPORT const char *wyrebox_plugin_version =
    DOVECOT_ABI_VERSION;

WYREBOX_DOVECOT_PLUGIN_EXPORT void
wyrebox_plugin_init (struct module *module)
{
  (void) module;
  mail_storage_class_register (&wyrebox_mail_storage_class);
}

WYREBOX_DOVECOT_PLUGIN_EXPORT void
wyrebox_plugin_deinit (void)
{
  wyrebox_dovecot_mailbox_list_restore_all_hooks ();
  g_clear_pointer (&wyrebox_dovecot_mailbox_list_hook_contexts,
      g_hash_table_unref);
  mail_storage_class_unregister (&wyrebox_mail_storage_class);
}
