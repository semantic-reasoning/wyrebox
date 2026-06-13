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

static struct mail_storage wyrebox_mail_storage_class;

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
        .mailbox_alloc = NULL,
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
