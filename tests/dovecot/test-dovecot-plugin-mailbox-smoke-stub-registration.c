#include "lib.h"
#include "mailbox-list-private.h"
#include "mail-storage-private.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct mail_storage *wyrebox_dovecot_loader_shim_mail_storage_class;
int wyrebox_dovecot_loader_shim_mail_storage_class_register_calls;
int wyrebox_dovecot_loader_shim_mail_storage_class_unregister_calls;

struct mailbox_list_sink
{
  struct mailbox_list list;
  struct mailbox_list_sink_entry *entries;
  unsigned int n_entries;
  unsigned int capacity;
  bool fail_next_publish;
  unsigned int original_iter_init_calls;
  unsigned int original_iter_next_calls;
  unsigned int original_iter_deinit_calls;
  unsigned int original_deinit_calls;
};

static struct mailbox_list_sink *
mailbox_list_sink_from_list (const struct mailbox_list *list)
{
  return (struct mailbox_list_sink *) ((char *) list
      - offsetof (struct mailbox_list_sink, list));
}

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

static struct mailbox_list_iterate_context *
mailbox_list_sink_original_iter_init (struct mailbox_list *list,
    const char *const *patterns, enum mailbox_list_iter_flags flags)
{
  struct mailbox_list_iterate_context *ctx;
  struct mailbox_list_sink *sink;

  (void) patterns;

  sink = mailbox_list_sink_from_list (list);
  sink->original_iter_init_calls++;

  ctx = calloc (1, sizeof (*ctx));
  if (ctx == NULL)
    return NULL;

  ctx->list = list;
  ctx->flags = flags;
  return ctx;
}

static const struct mailbox_info *
mailbox_list_sink_original_iter_next (struct mailbox_list_iterate_context *ctx)
{
  struct mailbox_list_sink *sink;

  sink = mailbox_list_sink_from_list (ctx->list);
  sink->original_iter_next_calls++;
  return NULL;
}

static int
mailbox_list_sink_original_iter_deinit (struct
    mailbox_list_iterate_context *ctx)
{
  struct mailbox_list_sink *sink;

  sink = mailbox_list_sink_from_list (ctx->list);
  sink->original_iter_deinit_calls++;
  free (ctx);
  return 0;
}

static void
mailbox_list_sink_original_deinit (struct mailbox_list *list)
{
  struct mailbox_list_sink *sink;

  sink = mailbox_list_sink_from_list (list);
  sink->original_deinit_calls++;
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
  struct mailbox_list_sink *sink;

  sink = calloc (1, sizeof (*sink));
  if (sink == NULL)
    return NULL;

  sink->list.v.deinit = mailbox_list_sink_original_deinit;
  sink->list.v.iter_init = mailbox_list_sink_original_iter_init;
  sink->list.v.iter_next = mailbox_list_sink_original_iter_next;
  sink->list.v.iter_deinit = mailbox_list_sink_original_iter_deinit;
  return &sink->list;
}

void
mailbox_list_sink_free (struct mailbox_list *list)
{
  struct mailbox_list_sink *sink;
  unsigned int i;

  if (list == NULL)
    return;

  sink = mailbox_list_sink_from_list (list);
  for (i = 0; i < sink->n_entries; i++)
    mailbox_list_sink_entry_clear (&sink->entries[i]);

  free (sink->entries);
  free (sink);
}

void
mailbox_list_sink_fail_next_publish (struct mailbox_list *list)
{
  if (list != NULL) {
    mailbox_list_sink_from_list (list)->fail_next_publish = true;
  }
}

bool
mailbox_list_sink_publish_entry (struct mailbox_list *list,
    const char *name, char hierarchy_delimiter, bool selectable,
    enum mailbox_list_child_state child_state, const char *special_use)
{
  struct mailbox_list_sink *sink;
  struct mailbox_list_sink_entry *entry;

  if (list == NULL || name == NULL)
    return false;

  sink = mailbox_list_sink_from_list (list);
  if (sink->fail_next_publish) {
    sink->fail_next_publish = false;
    return false;
  }

  if (sink->n_entries == sink->capacity) {
    unsigned int new_capacity = sink->capacity == 0 ? 4 : sink->capacity * 2;
    struct mailbox_list_sink_entry *new_entries;

    new_entries = realloc (sink->entries,
        sizeof (struct mailbox_list_sink_entry) * new_capacity);
    if (new_entries == NULL)
      return false;

    sink->entries = new_entries;
    sink->capacity = new_capacity;
  }

  entry = &sink->entries[sink->n_entries];
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
  sink->n_entries++;
  return true;
}

unsigned int
mailbox_list_sink_get_count (const struct mailbox_list *list)
{
  return list != NULL ? mailbox_list_sink_from_list (list)->n_entries : 0;
}

const struct mailbox_list_sink_entry *
mailbox_list_sink_get_entry (const struct mailbox_list *list,
    unsigned int index)
{
  struct mailbox_list_sink *sink;

  if (list == NULL)
    return NULL;

  sink = mailbox_list_sink_from_list (list);
  if (index >= sink->n_entries)
    return NULL;

  return &sink->entries[index];
}

unsigned int
mailbox_list_sink_get_original_iter_init_calls (const struct mailbox_list *list)
{
  return list != NULL
      ? mailbox_list_sink_from_list (list)->original_iter_init_calls : 0;
}

unsigned int
mailbox_list_sink_get_original_iter_next_calls (const struct mailbox_list *list)
{
  return list != NULL
      ? mailbox_list_sink_from_list (list)->original_iter_next_calls : 0;
}

unsigned int
mailbox_list_sink_get_original_iter_deinit_calls (const struct
    mailbox_list *list)
{
  return list != NULL
      ? mailbox_list_sink_from_list (list)->original_iter_deinit_calls : 0;
}

unsigned int
mailbox_list_sink_get_original_deinit_calls (const struct mailbox_list *list)
{
  return list != NULL
      ? mailbox_list_sink_from_list (list)->original_deinit_calls : 0;
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
