#include "lib.h"
#include "mail-storage-private.h"

#include <stdlib.h>
#include <string.h>

struct mail_storage *wyrebox_dovecot_loader_shim_mail_storage_class;
int wyrebox_dovecot_loader_shim_mail_storage_class_register_calls;
int wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls;

struct mailbox_list
{
  struct mailbox_list_sink_entry *entries;
  unsigned int n_entries;
  unsigned int capacity;
  bool fail_next_publish;
};

static void
mailbox_list_sink_entry_clear (struct mailbox_list_sink_entry *entry)
{
  free ((char *) entry->name);
  free ((char *) entry->special_use);
  memset (entry, 0, sizeof (*entry));
}

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

struct mailbox_list *
mailbox_list_sink_alloc (void)
{
  return calloc (1, sizeof (struct mailbox_list));
}

void
mailbox_list_sink_free (struct mailbox_list *list)
{
  unsigned int i;

  if (list == NULL)
    return;

  for (i = 0; i < list->n_entries; i++)
    mailbox_list_sink_entry_clear (&list->entries[i]);

  free (list->entries);
  free (list);
}

void
mailbox_list_sink_fail_next_publish (struct mailbox_list *list)
{
  if (list != NULL)
    list->fail_next_publish = true;
}

bool
mailbox_list_sink_publish_entry (struct mailbox_list *list,
    const char *name, char hierarchy_delimiter, bool selectable,
    enum mailbox_list_child_state child_state, const char *special_use)
{
  struct mailbox_list_sink_entry *entry;

  if (list == NULL || name == NULL)
    return false;

  if (list->fail_next_publish) {
    list->fail_next_publish = false;
    return false;
  }

  if (list->n_entries == list->capacity) {
    unsigned int new_capacity = list->capacity == 0 ? 4 : list->capacity * 2;
    struct mailbox_list_sink_entry *new_entries;

    new_entries = realloc (list->entries,
        sizeof (struct mailbox_list_sink_entry) * new_capacity);
    if (new_entries == NULL)
      return false;

    list->entries = new_entries;
    list->capacity = new_capacity;
  }

  entry = &list->entries[list->n_entries];
  memset (entry, 0, sizeof (*entry));
  entry->name = p_strdup (NULL, name);
  if (entry->name == NULL)
    return false;

  if (special_use != NULL) {
    entry->special_use = p_strdup (NULL, special_use);
    if (entry->special_use == NULL) {
      mailbox_list_sink_entry_clear (entry);
      return false;
    }
  }

  entry->hierarchy_delimiter = hierarchy_delimiter;
  entry->selectable = selectable;
  entry->child_state = child_state;
  list->n_entries++;
  return true;
}

unsigned int
mailbox_list_sink_get_count (const struct mailbox_list *list)
{
  return list != NULL ? list->n_entries : 0;
}

const struct mailbox_list_sink_entry *
mailbox_list_sink_get_entry (const struct mailbox_list *list,
    unsigned int index)
{
  if (list == NULL || index >= list->n_entries)
    return NULL;

  return &list->entries[index];
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
