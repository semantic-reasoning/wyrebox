#include "wyrebox-fact-reconciliation.h"

#include <string.h>

#include <gio/gio.h>

static void
fact_record_free (gpointer data)
{
  WyreboxFactRecord *record = data;

  if (record == NULL)
    return;

  wyrebox_fact_record_clear (record);
  g_free (record);
}

static char *
build_fact_identity_key (const WyreboxFactRecord *record, GError **error)
{
  g_autoptr (GString) key = NULL;

  if (record == NULL) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "fact record is required");
    return NULL;
  }

  if (record->predicate == NULL || record->predicate[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "fact predicate is required");
    return NULL;
  }

  if (record->args == NULL) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "fact args are required");
    return NULL;
  }

  if (record->source == NULL || record->source[0] == '\0') {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "fact source is required");
    return NULL;
  }

  key = g_string_new (NULL);
  g_string_append_printf (key, "P%" G_GSIZE_FORMAT ":%s",
      strlen (record->predicate), record->predicate);
  for (guint i = 0; record->args[i] != NULL; i++) {
    if (record->args[i][0] == '\0') {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT, "fact args must not be empty");
      return NULL;
    }
    g_string_append_printf (key, "A%" G_GSIZE_FORMAT ":%s",
        strlen (record->args[i]), record->args[i]);
  }
  g_string_append_printf (key, "S%" G_GSIZE_FORMAT ":%s",
      strlen (record->source), record->source);

  return g_string_free (g_steal_pointer (&key), FALSE);
}

static gboolean
validate_active_fact (const WyreboxFactRecord *record,
    const char *snapshot_name, guint index, GError **error)
{
  if (record == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "%s fact at index %u is required", snapshot_name, index);
    return FALSE;
  }

  if (record->created_at_unix_us == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "%s fact at index %u has no creation timestamp", snapshot_name, index);
    return FALSE;
  }

  if (record->retracted_at_unix_us != 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "%s fact at index %u is not active", snapshot_name, index);
    return FALSE;
  }

  return TRUE;
}

static GHashTable *
build_identity_set (GPtrArray *records,
    const char *snapshot_name, GError **error)
{
  g_autoptr (GHashTable) identities = NULL;

  identities = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

  for (guint i = 0; i < records->len; i++) {
    const WyreboxFactRecord *record = g_ptr_array_index (records, i);
    g_autofree char *key = NULL;

    if (!validate_active_fact (record, snapshot_name, i, error))
      return NULL;

    key = build_fact_identity_key (record, error);
    if (key == NULL)
      return NULL;

    if (g_hash_table_contains (identities, key)) {
      g_set_error (error,
          G_IO_ERROR,
          G_IO_ERROR_INVALID_ARGUMENT,
          "%s fact at index %u duplicates an earlier fact identity",
          snapshot_name, i);
      return NULL;
    }

    g_hash_table_add (identities, g_steal_pointer (&key));
  }

  return g_steal_pointer (&identities);
}

static WyreboxFactRecord *
copy_fact_record (const WyreboxFactRecord *record, GError **error)
{
  g_auto (WyreboxFactRecord) copy = { 0 };
  WyreboxFactRecord *owned = NULL;

  if (!wyrebox_fact_record_init (&copy,
          record->predicate,
          (const char *const *) record->args,
          record->source,
          record->confidence_ppm, record->created_at_unix_us, error))
    return NULL;

  owned = g_new0 (WyreboxFactRecord, 1);
  *owned = copy;
  memset (&copy, 0, sizeof (copy));

  return owned;
}

static gboolean
append_retractions (GPtrArray *changes,
    GPtrArray *previous_facts,
    GHashTable *new_identities, guint64 retracted_at_unix_us, GError **error)
{
  for (guint i = 0; i < previous_facts->len; i++) {
    const WyreboxFactRecord *record = g_ptr_array_index (previous_facts, i);
    g_autofree char *key = NULL;
    WyreboxFactRecord *copy = NULL;

    key = build_fact_identity_key (record, error);
    if (key == NULL)
      return FALSE;

    if (g_hash_table_contains (new_identities, key))
      continue;

    copy = copy_fact_record (record, error);
    if (copy == NULL)
      return FALSE;

    if (!wyrebox_fact_record_mark_retracted (copy, retracted_at_unix_us, error)) {
      fact_record_free (copy);
      return FALSE;
    }

    g_ptr_array_add (changes, copy);
  }

  return TRUE;
}

static gboolean
append_inserts (GPtrArray *changes,
    GPtrArray *new_facts, GHashTable *previous_identities, GError **error)
{
  for (guint i = 0; i < new_facts->len; i++) {
    const WyreboxFactRecord *record = g_ptr_array_index (new_facts, i);
    g_autofree char *key = NULL;
    WyreboxFactRecord *copy = NULL;

    key = build_fact_identity_key (record, error);
    if (key == NULL)
      return FALSE;

    if (g_hash_table_contains (previous_identities, key))
      continue;

    copy = copy_fact_record (record, error);
    if (copy == NULL)
      return FALSE;

    g_ptr_array_add (changes, copy);
  }

  return TRUE;
}

GPtrArray *
wyrebox_fact_reconciliation_reconcile (GPtrArray *previous_facts,
    GPtrArray *new_facts, guint64 retracted_at_unix_us, GError **error)
{
  g_autoptr (GHashTable) previous_identities = NULL;
  g_autoptr (GHashTable) new_identities = NULL;
  g_autoptr (GPtrArray) changes = NULL;

  g_return_val_if_fail (error == NULL || *error == NULL, NULL);

  if (previous_facts == NULL) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "previous facts array is required");
    return NULL;
  }

  if (new_facts == NULL) {
    g_set_error (error,
        G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "new facts array is required");
    return NULL;
  }

  if (retracted_at_unix_us == 0) {
    g_set_error (error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT, "retraction timestamp is required");
    return NULL;
  }

  previous_identities = build_identity_set (previous_facts, "previous", error);
  if (previous_identities == NULL)
    return NULL;

  new_identities = build_identity_set (new_facts, "new", error);
  if (new_identities == NULL)
    return NULL;

  changes = g_ptr_array_new_with_free_func (fact_record_free);

  if (!append_retractions (changes,
          previous_facts, new_identities, retracted_at_unix_us, error))
    return NULL;

  if (!append_inserts (changes, new_facts, previous_identities, error))
    return NULL;

  return g_steal_pointer (&changes);
}
