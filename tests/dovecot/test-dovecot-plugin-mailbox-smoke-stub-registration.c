#include "lib.h"
#include "mail-storage-private.h"

#include <stdlib.h>
#include <string.h>

struct mail_storage *wyrebox_dovecot_loader_shim_mail_storage_class;
int wyrebox_dovecot_loader_shim_mail_storage_class_register_calls;
int wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls;

void
mail_storage_class_register (struct mail_storage *storage_class)
{
  wyrebox_dovecot_loader_shim_mail_storage_class = storage_class;
  ++wyrebox_dovecot_loader_shim_mail_storage_class_register_calls;
}

void
mail_storage_class_unregister (struct mail_storage *storage_class)
{
  (void) storage_class;
  ++wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls;
}

const char *
mailbox_list_get_storage_name (struct mailbox_list *list, const char *vname)
{
  (void) list;
  (void) vname;
  return NULL;
}

void
mail_storage_set_error (struct mail_storage *storage,
    enum mail_error error, const char *string)
{
  (void) storage;
  (void) error;
  (void) string;
}

pool_t
pool_alloconly_create (const char *name, size_t size)
{
  (void) name;
  (void) size;
  return malloc (1);
}

void
pool_unref (pool_t *pool)
{
  free (*pool);
  *pool = NULL;
}

void *
p_malloc (pool_t pool, size_t size)
{
  (void) pool;
  return calloc (1, size);
}

void *
p_strdup (pool_t pool, const char *str)
{
  size_t size;
  char *copy;

  (void) pool;
  if (str == NULL)
    return NULL;

  size = strlen (str) + 1;
  copy = malloc (size);
  if (copy == NULL)
    return NULL;
  memcpy (copy, str, size);
  return copy;
}

struct event *
event_create (struct event *parent)
{
  (void) parent;
  return malloc (1);
}

void
event_unref (struct event **event)
{
  free (*event);
  *event = NULL;
}

void
p_array_init (array_t *array, pool_t pool, unsigned int count)
{
  (void) pool;
  (void) count;
  array->dummy = NULL;
}
