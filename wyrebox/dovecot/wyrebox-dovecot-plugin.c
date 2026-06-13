#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#include "lib.h"
#include "mail-namespace.h"
#include "mail-storage.h"
#include "mail-storage-private.h"
#include "mail-user.h"
#include "module-dir.h"
#include "wyrebox-daemon-mailbox-select-result.h"
#include "wyrebox-dovecot-daemon-client.h"

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
};

static struct mail_storage wyrebox_mail_storage_class;

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
  wyrebox_daemon_mailbox_select_result_clear (&wbox->select_result);
  wbox->select_result_valid = 0;
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

  wyrebox_daemon_mailbox_select_result_clear (&wbox->select_result);
  wbox->select_result_valid = 0;

  if (!wyrebox_dovecot_daemon_client_select_mailbox (storage->socket_path,
          storage->account_identity, box->vname, &select_result, error)) {
    return FALSE;
  }

  if (!wyrebox_daemon_mailbox_select_result_init (&wbox->select_result,
          select_result.kind, select_result.mailbox_id,
          select_result.mailbox_name, select_result.uid_validity,
          select_result.uid_next, error)) {
    return FALSE;
  }

  wbox->select_result_valid = 1;
  return TRUE;
}

static void
wyrebox_dovecot_mailbox_free (struct mailbox *box)
{
  struct wyrebox_dovecot_mailbox *wbox = (struct wyrebox_dovecot_mailbox *) box;

  wyrebox_daemon_mailbox_select_result_clear (&wbox->select_result);
  wbox->select_result_valid = 0;
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

  status_r->messages = 0;
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

  wstorage->socket_path = wyrebox_dovecot_strdup ("/run/wyrebox/wyrebox.sock");
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

static struct mail_storage wyrebox_mail_storage_class = {
  .name = "wyrebox",
  .class_flags = 0,
  .v = {
        .alloc = wyrebox_dovecot_storage_alloc,
        .create = wyrebox_dovecot_storage_create,
        .destroy = wyrebox_dovecot_storage_destroy,
        .add_list = NULL,
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
  mail_storage_class_unregister (&wyrebox_mail_storage_class);
}
