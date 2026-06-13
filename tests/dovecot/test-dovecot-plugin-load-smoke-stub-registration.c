#include "config.h"

#include "lib.h"
#include "mail-storage.h"

int wyrebox_dovecot_loader_shim_mail_storage_class_register_calls;
int wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls;

void
mail_storage_class_register (struct mail_storage *storage_class)
{
  (void) storage_class;
  ++wyrebox_dovecot_loader_shim_mail_storage_class_register_calls;
}

void
mail_storage_class_unregister (struct mail_storage *storage_class)
{
  (void) storage_class;
  ++wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls;
}
