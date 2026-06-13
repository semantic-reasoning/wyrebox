#include <stddef.h>

#include "config.h"

#include "lib.h"
#include "mail-storage.h"
#include "mail-storage-private.h"
#include "module-dir.h"

static struct mail_storage wyrebox_mail_storage_class = {
  .name = "wyrebox",
  .class_flags = 0,
  .v = {
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
