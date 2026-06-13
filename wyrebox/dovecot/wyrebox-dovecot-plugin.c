#include <stddef.h>

#include "config.h"

#include "lib.h"
#include "mail-storage.h"
#include "mail-storage-private.h"
#include "module-dir.h"

struct wyrebox_dovecot_storage
{
  struct mail_storage storage;
};

struct wyrebox_dovecot_mailbox
{
  struct mailbox mailbox;
};

static struct mail_storage wyrebox_mail_storage_class;

static void
wyrebox_dovecot_mailbox_free (struct mailbox *box)
{
  event_unref (&box->event);
}

static int
wyrebox_dovecot_mailbox_open (struct mailbox *box)
{
  mail_storage_set_error (box->storage, MAIL_ERROR_NOTPOSSIBLE,
      "WyreBox mailbox open is not implemented yet");
  return -1;
}

static int
wyrebox_dovecot_mailbox_get_status (struct mailbox *box,
    enum mailbox_status_items items, struct mailbox_status *status_r)
{
  (void) box;
  (void) items;

  status_r->messages = 0;
  status_r->uidvalidity = 1;
  status_r->uidnext = 1;
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
  (void) storage;
  (void) ns;
  (void) error_r;
  return 0;
}

static void
wyrebox_dovecot_storage_destroy (struct mail_storage *storage)
{
  (void) storage;
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
